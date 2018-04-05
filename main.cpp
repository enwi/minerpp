#define _FILE_OFFSET_BITS 64    // needs to be defined first

#include <atomic>       // needed for atomic
#include <ctime>        // needed for time_t
#include <fcntl.h>      // needed fot fcntl
#include <fstream>      // needed for ifstream
#include <iostream>
#include <limits.h>
#include <mutex>        // needed for mutex
#include <string>
#include <signal.h>     // needed for SignalHandler
#include <thread>       // needed for thread
#include <unistd.h>     // needed for readlink and lseek64

#include <sys/socket.h>
#ifdef __linux__
#include <sys/statfs.h> // needed for statfs  // not working on macos
#include <sys/stat.h>
#endif
#include <sys/mman.h>   // needed for mmap and munmap
#include <sys/types.h>  // needed for lseek64
#include <errno.h> //For errno - the error number
#include <netdb.h> //hostent
#include <arpa/inet.h>

#include <experimental/filesystem>

#ifdef __APPLE__
#include <libproc.h>    // mac: needed for proc_pidpath
#endif

#include "json.hpp"
// use c library
extern "C"
{
    #include "picohttpparser.h"
    #include "sph_shabal.h"
};

#include "consoleColor.h"

#ifndef INVALID_SOCKET
const int INVALID_SOCKET = -1;
const int SOCKET_ERROR = -1;
const int INVALID_HANDLE_VALUE = -1;
#endif

#ifndef lseek64
#define lseek64(handle,offset,whence) lseek(handle,offset,whence)
#endif

#ifdef __AVX2__
	char const *const version = "v1.170820_AVX2";
#else
	#ifdef __AVX__
		char const *const version = "v1.170820_AVX";
	#else
		char const *const version = "v1.170820";
	#endif
#endif

// needed for lseek64
#ifndef _LARGEFILE64_SOURCE
#define _LARGEFILE64_SOURCE
#endif

enum class MinerMode
{
    unknown = 0,
	solo,
	pool
};

std::string MinerModeToStr(MinerMode mode)
{
    switch (mode)
    {
        case MinerMode::solo:
            return "solo";
        case MinerMode::pool:
            return "pool";
        default:
            return "unknown";
    }
}

uint64_t GetFileSize(std::string path)
{
    struct stat stat_buf;

    if (stat(path.c_str(), &stat_buf) != 0) // |TODO maybe use stat64 for very large files
    {
        // error happens, just quits here
        console::SetColor(console::color::FG_RED);
        std::cout << "[GetFileSize] Error could not run stat: " << std::strerror(errno) << std::endl;
        console::SetColor(console::color::RESET);
        return 0;
    }
    return stat_buf.st_size;
}

bool GetAvailableSpace(const char* path, uint32_t &spc, uint32_t &sector_size, uint32_t &free_blocks, uint32_t &total_blocks)
{
    struct statfs statfs_buf;

    if (statfs(path, &statfs_buf) != 0)
    {
        // error happens, just quits here
        console::SetColor(console::color::FG_RED);
        std::cout << "[GetAvailableSpace] Error could not run statfs: " << std::strerror(errno) << std::endl;
        console::SetColor(console::color::RESET);
        return false;
    }
    // \TODO find equivalent for sectors per cluster (spc)
    sector_size  = statfs_buf.f_bsize;
    free_blocks  = statfs_buf.f_bfree;
    total_blocks = statfs_buf.f_blocks;
    // the available size is f_bsize * f_bavail
    return true;
}

// required options
MinerMode mode;             //!< The mode of the miner
std::string server_address; //!< The ip-address or hostname of the server
std::string server_ip;      //!< The resolved ip-address of the server
std::string server_port;    //!< The port of the server

std::string updater_address;
std::string updater_ip;
std::string updater_port;

std::string info_address;
std::string info_port;

std::vector<std::string> plot_paths;    //!< Vector of paths to plotfiles
uint64_t total_plotfile_size = 0;

uint64_t cache_size;

size_t update_interval;
size_t send_interval;

uint64_t target_deadline;

// optional options
bool use_debug = false;
bool use_hdd_wakeup = false;
bool enable_proxy = false;
bool show_winner = false;
uint16_t proxy_port = 0;
std::string passphrase;

// info
uint8_t network_quality = 100;

// data
uint64_t block_height = 0;
uint64_t base_target = 0;
std::string signature;
std::string signature_str;
std::string old_signature;

uint64_t target_deadline_info;

uint32_t scoop;     //!< \TODO or uint16_t??
uint64_t deadline;
uint64_t my_target_deadline = std::numeric_limits<uint64_t>::max(); // 4294967295;

std::chrono::system_clock::time_point cur_time;

// \TODO remove u_long
std::map <u_long, uint64_t> satellite_size;

// structs
struct Plotfile
{
	std::string path;
	std::string name;
	uint64_t size;// = 0;
	uint64_t key;
	uint64_t start_nonce;
	uint64_t nonces;
	uint64_t stagger;
};

struct WorkerProgress
{
	size_t number;
	uint64_t reads_bytes;
	bool is_alive;
};

struct Share
{
	std::string file_name;
	uint64_t account_id;// = 0;
	uint64_t best;// = 0;
	uint64_t nonce;// = 0;
};

struct Session
{
#ifdef __WIN32
	SOCKET Socket;
#elif defined(__linux__) || defined(__APPLE__)
	int socket;
#endif
	uint64_t deadline;
	Share body;
};

struct Best
{
	uint64_t account_id;// = 0;
	uint64_t best;// = 0;
	uint64_t nonce;// = 0;
	uint64_t deadline;// = 0;
	uint64_t target_deadline;// = 0;
};

// best related stuff
std::vector<Best> bests;

// share related stuff
std::vector<Share> shares;

// session related stuff
std::vector<Session> sessions;

// thread related stuff
volatile std::atomic<bool> stop_all_threads;

std::thread updater_thread;

volatile std::atomic<bool> stop_local_threads;
std::vector<std::thread> worker_threads;
std::vector<WorkerProgress> worker_progress;
//std::mutex shutdown_all_mutex;

// mutexes
std::mutex bests_mutex;
std::mutex sessions_mutex;
std::mutex shares_mutex;
std::mutex signature_mutex;


std::string GetExePath()
{
    std::string path = "";
#ifdef __APPLE__

    pid_t pid = getpid();
    char result[PROC_PIDPATHINFO_MAXSIZE];
    ssize_t count = proc_pidpath (pid, result, sizeof(result));

#elif __linux__

    char result[ PATH_MAX ];
    ssize_t count = readlink( "/proc/self/exe", result, PATH_MAX );

#endif

    path = std::string(result, (count > 0) ? count : 0);
    path = path.substr(0, path.find_last_of("/\\"));
    return path;
}

const std::string HostnameToIP(const std::string hostname)
{
    int sockfd;
    struct addrinfo hints, *servinfo, *p;
    struct sockaddr_in *h;
    int rv;
    std::string ip;

    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC; // use AF_INET6 to force IPv6
    hints.ai_socktype = SOCK_STREAM;

    if ( (rv = getaddrinfo( hostname.c_str() , "http" , &hints , &servinfo)) != 0)
    {
        console::SetColor(console::color::FG_RED);
        std::cout << "[HostnameToIP] Error: getaddrinfo: " << gai_strerror(rv) << std::endl;
        console::SetColor(console::color::RESET);
        return "";
    }

    // loop through all the results and connect to the first we can
    for(p = servinfo; p != NULL; p = p->ai_next)
    {
        h = (struct sockaddr_in *) p->ai_addr;
        ip = std::string(inet_ntoa( h->sin_addr ));
        if(ip.compare("0.0.0.0") != 0)
        {
            return ip;
        }
    }

    freeaddrinfo(servinfo); // all done with this structure
    return std::string(ip);
}

bool LoadConfig(std::string file)
{
    std::ifstream config_file(file.c_str());
    bool ret = false;
	if (config_file.is_open())
	{
        nlohmann::json config;
        config_file >> config;
        config_file.close();

        if(config.is_object())
        {
            uint8_t errors = 0;

            // check mode
            if(config["Mode"].empty())
            {
                console::SetColor(console::color::FG_RED);
                std::cout << "[loadConfig] Error you need to specify the \"Mode\" of the miner!\n";
                console::SetColor(console::color::RESET);
                ++errors;
            }
            else if(!config["Mode"].is_string())
            {
                console::SetColor(console::color::FG_RED);
                std::cout << "[loadConfig] Error you need to specify the \"Mode\" as string!\n";
                console::SetColor(console::color::RESET);
                ++errors;
            }
            else
            {
                if(config["Mode"] == "solo")
                {
                    mode = MinerMode::solo;
                }
                else if(config["Mode"] == "pool")
                {
                    mode = MinerMode::pool;
                }
                else
                {
                    console::SetColor(console::color::FG_RED);
                    std::cout << "[loadConfig] Error the specified \"Mode\" "<< config["Mode"] <<" is invalid!\n";
                    console::SetColor(console::color::RESET);
                    ++errors;
                }
            }

            // check server ip
            if(config["Server"].empty())
            {
                console::SetColor(console::color::FG_RED);
                std::cout << "[loadConfig] Error you need to specify the \"Server\" hostname or ip-address!\n";
                console::SetColor(console::color::RESET);
                ++errors;
            }
            else if(!config["Server"].is_string())
            {
                console::SetColor(console::color::FG_RED);
                std::cout << "[loadConfig] Error you need to specify the \"Server\" hostname or ip-address as string!\n";
                console::SetColor(console::color::RESET);
                ++errors;
            }
            else
            {
                server_address = config["Server"];
            }

            // check server port
            if(config["Port"].empty())
            {
                console::SetColor(console::color::FG_RED);
                std::cout << "[loadConfig] Error you need to specify the \"Port\" of the server!\n";
                console::SetColor(console::color::RESET);
                ++errors;
            }
            else if(!config["Port"].is_number())
            {
                console::SetColor(console::color::FG_RED);
                std::cout << "[loadConfig] Error you need to specify the \"Port\" of the server as a number!\n";
                console::SetColor(console::color::RESET);
                ++errors;
            }
            else
            {
                uint16_t port = config["Port"];
                server_port = std::to_string(port);
            }

            // check Plotfile paths
            if(config["Paths"].empty())
            {
                console::SetColor(console::color::FG_RED);
                std::cout << "[loadConfig] Error you need to specify the \"Paths\" to your plotfiles!\n";
                console::SetColor(console::color::RESET);
                ++errors;
            }
            else if(!config["Paths"].is_array())
            {
                console::SetColor(console::color::FG_RED);
                std::cout << "[loadConfig] Error you need to specify the \"Paths\" as an array/list!\n";
                console::SetColor(console::color::RESET);
                ++errors;
            }
            else
            {
                for (auto& path : config["Paths"])
                {
                    plot_paths.push_back(path);
                }
            }

            // check cache size \TODO Is it optional?
            if(config["CacheSize"].empty())
            {
                console::SetColor(console::color::FG_RED);
                std::cout << "[loadConfig] Error you need to specify the \"CacheSize\"!\n";
                console::SetColor(console::color::RESET);
                ++errors;
            }
            else if(!config["CacheSize"].is_number())
            {
                console::SetColor(console::color::FG_RED);
                std::cout << "[loadConfig] Error you need to specify the \"CacheSize\" as a number!\n";
                console::SetColor(console::color::RESET);
                ++errors;
            }
            else
            {
                cache_size = config["CacheSize"];
            }

            // check use hdd wakeup
            if(!config["UseHDDWakeUp"].empty())
            {
                if(!config["UseHDDWakeUp"].is_boolean())
                {
                    console::SetColor(console::color::FG_RED);
                    std::cout << "[loadConfig] Error you need to specify \"UseHDDWakeUp\" as a boolean!\n";
                    console::SetColor(console::color::RESET);
                    ++errors;
                }
                else
                {
                    use_hdd_wakeup = config["UseHDDWakeUp"];
                }
            }

            // check send interval
            if(config["SendInterval"].empty())
            {
                console::SetColor(console::color::FG_RED);
                std::cout << "[loadConfig] Error you need to specify the \"SendInterval\"!\n";
                console::SetColor(console::color::RESET);
                ++errors;
            }
            else if(!config["SendInterval"].is_number())
            {
                console::SetColor(console::color::FG_RED);
                std::cout << "[loadConfig] Error you need to specify the \"SendInterval\" as a number!\n";
                console::SetColor(console::color::RESET);
                ++errors;
            }
            else
            {
                send_interval = config["SendInterval"];
            }

            // check update interval
            if(config["UpdateInterval"].empty())
            {
                console::SetColor(console::color::FG_RED);
                std::cout << "[loadConfig] Error you need to specify the \"UpdateInterval\"!\n";
                console::SetColor(console::color::RESET);
                ++errors;
            }
            else if(!config["UpdateInterval"].is_number())
            {
                console::SetColor(console::color::FG_RED);
                std::cout << "[loadConfig] Error you need to specify the \"UpdateInterval\" as a number!\n";
                console::SetColor(console::color::RESET);
                ++errors;
            }
            else
            {
                update_interval = config["UpdateInterval"];
            }

            // check debug
            if(!config["Debug"].empty())
            {
                if(!config["Debug"].is_boolean())
                {
                    console::SetColor(console::color::FG_RED);
                    std::cout << "[loadConfig] Error you need to specify \"Debug\" as a boolean!\n";
                    console::SetColor(console::color::RESET);
                    ++errors;
                }
                else
                {
                    use_debug = config["Debug"];
                }
            }

            // check updater address
            if(config["UpdaterAddr"].empty())
            {
                updater_address = server_address;
            }
            else if(!config["UpdaterAddr"].is_string())
            {
                console::SetColor(console::color::FG_RED);
                std::cout << "[loadConfig] Error you need to specify the \"UpdaterAddr\" as a string!\n";
                console::SetColor(console::color::RESET);
                ++errors;
            }
            else
            {
                updater_address = config["UpdaterAddr"];
            }

            // check updater port
            if(config["UpdaterPort"].empty())
            {
                updater_port = server_port;
            }
            else if(!config["UpdaterPort"].is_string())
            {
                console::SetColor(console::color::FG_RED);
                std::cout << "[loadConfig] Error you need to specify the \"UpdaterPort\" as a string!\n";
                console::SetColor(console::color::RESET);
                ++errors;
            }
            else
            {
                updater_port = config["UpdaterPort"];
            }

            // check info address
            if(config["InfoAddr"].empty())
            {
                info_address = updater_address;
            }
            else if(!config["InfoAddr"].is_string())
            {
                console::SetColor(console::color::FG_RED);
                std::cout << "[loadConfig] Error you need to specify the \"InfoAddr\" as a string!\n";
                console::SetColor(console::color::RESET);
                ++errors;
            }
            else
            {
                info_address = config["InfoAddr"];
            }

            // check info port
            if(config["InfoPort"].empty())
            {
                info_port = updater_port;
            }
            else if(!config["InfoPort"].is_string())
            {
                console::SetColor(console::color::FG_RED);
                std::cout << "[loadConfig] Error you need to specify the \"InfoPort\" as a string!\n";
                console::SetColor(console::color::RESET);
                ++errors;
            }
            else
            {
                info_port = config["InfoPort"];
            }

            // check enable proxy
            if(!config["EnableProxy"].empty())
            {
                if(!config["EnableProxy"].is_boolean())
                {
                    console::SetColor(console::color::FG_RED);
                    std::cout << "[loadConfig] Error you need to specify \"EnableProxy\" as a boolean!\n";
                    console::SetColor(console::color::RESET);
                    ++errors;
                }
                else
                {
                    enable_proxy = config["EnableProxy"];
                }
            }

            // check proxy port
            if(!config["ProxyPort"].empty())
            {
                if(!config["ProxyPort"].is_number())
                {
                    console::SetColor(console::color::FG_RED);
                    std::cout << "[loadConfig] Error you need to specify the \"ProxyPort\" as a number!\n";
                    console::SetColor(console::color::RESET);
                    ++errors;
                }
                else
                {
                    if(!enable_proxy)
                    {
                        console::SetColor(console::color::FG_YELLOW);
                        std::cout << "[loadConfig] Warning \"ProxyPort\" specified, but \"EnableProxy\" disabled.\n";
                        console::SetColor(console::color::RESET);
                    }
                    proxy_port = config["ProxyPort"];
                }
            }
            else if(enable_proxy)
            {
                console::SetColor(console::color::FG_RED);
                std::cout << "[loadConfig] Error no \"ProxyPort\" specified, but \"EnableProxy\" enabled!\n";
                console::SetColor(console::color::RESET);
                ++errors;
            }

            if (!config["ShowWinner"].empty())
            {
                if(config["ShowWinner"].is_boolean())
                {
                    show_winner = config["ShowWinner"];
                }
                else
                {
                    console::SetColor(console::color::FG_RED);
                    std::cout << "[loadConfig] Error you need to specify \"ShowWinner\" as a boolean!\n";
                    console::SetColor(console::color::RESET);
                    ++errors;
                }
            }

            // check target deadline
            if(config["TargetDeadline"].empty())
            {
                console::SetColor(console::color::FG_RED);
                std::cout << "[loadConfig] Error you need to specify the \"TargetDeadline\"!\n";
                console::SetColor(console::color::RESET);
                ++errors;
            }
            else if(!config["TargetDeadline"].is_number())
            {
                console::SetColor(console::color::FG_RED);
                std::cout << "[loadConfig] Error you need to specify the \"TargetDeadline\" as a number!\n";
                console::SetColor(console::color::RESET);
                ++errors;
            }
            else
            {
                target_deadline = config["TargetDeadline"];
            }

            //! \TODO Add UseBoost!

            // check if errors occured
            if(!errors)
            {
                ret = true;
            }
        }
    }
    else
    {
        console::SetColor(console::color::FG_RED);
        std::cout << "[loadConfig] Error could not open config file \"" << file << "\"!\n";
        console::SetColor(console::color::RESET);
    }
    return ret;
}

bool ReadPassphrase(std::string file)
{
    std::ifstream passphrase_file(file.c_str());
    bool ret = false;
	if (passphrase_file.is_open())
	{
        std::getline(passphrase_file, passphrase);
        passphrase_file.close();
        std::transform(passphrase.begin(), passphrase.end(), passphrase.begin(), [](char ch) {
            return ch == ' ' ? '+' : ch;
        });
        while(passphrase.back() == '+')
        {
            passphrase.pop_back();
        }
        ret = true;
    }
    else
    {
        console::SetColor(console::color::FG_RED);
        std::cout << "[loadConfig] Error could not open passphrase file \"" << file << "\"!\n";
        console::SetColor(console::color::RESET);
    }
    return ret;
}

void SplitStringIntoVector(std::string str, char delimiter, std::vector<std::string> &v)
{
    size_t pos;
    while ((pos = str.find(delimiter)) != std::string::npos)
    {
        v.push_back(str.substr(0, pos));
        str.erase(0, pos + 1);
    }
    if(str.size())  // don't forget last part after last delimiter
    {
        v.push_back(str);
    }
}

size_t GetPlotfiles(const std::string path, std::vector<Plotfile> &files)
{
    size_t count = 0;
    const std::experimental::filesystem::directory_iterator end{};
    std::experimental::filesystem::directory_iterator fs_iter{};
    try
    {
        fs_iter = std::experimental::filesystem::directory_iterator{path};
    }
    catch(std::experimental::filesystem::filesystem_error& e)
    {
        console::SetColor(console::color::FG_YELLOW);
        std::cout << "[GetPlotfiles] Warning could not open directory. Reason: " << e.what() << std::endl;
        console::SetColor(console::color::RESET);
        return 0;
    }
    while(fs_iter != end)
    {
        if( std::experimental::filesystem::is_regular_file(*fs_iter) || std::experimental::filesystem::is_other(*fs_iter) )
        {
            std::string filename = fs_iter->path().filename().string();
            std::vector<std::string> split_filename;
            SplitStringIntoVector(filename, '_', split_filename);
            if(split_filename.size() == 4)
            {
                try
                {
                    std::string tmp_file_path = fs_iter->path().string();
                    tmp_file_path = tmp_file_path.substr(0, tmp_file_path.find_last_of("/\\")+1);
                    files.push_back(Plotfile{
                        tmp_file_path,
                        filename,
                        // Filesize in bits
                        //std::experimental::filesystem::file_size(fs_iter->path()), // this does not work for is_other files
                        GetFileSize(fs_iter->path().string()),
                        std::stoull(split_filename[0]),
                        std::stoull(split_filename[1]),
                        std::stoull(split_filename[2]),
                        std::stoull(split_filename[3])
                    });
                }
                catch(std::invalid_argument& e)
                {
                    console::SetColor(console::color::FG_YELLOW);
                    std::cout << "[GetPlotfiles] Warning could not convert string to int. Reason: " << e.what() << std::endl;
                    console::SetColor(console::color::RESET);
                }
                catch(std::out_of_range& e)
                {
                    console::SetColor(console::color::FG_YELLOW);
                    std::cout << "[GetPlotfiles] Warning converted string out of the range. Reason: " << e.what() << std::endl;
                    console::SetColor(console::color::RESET);
                }
                ++count;
            }
            else
            {
                //std::cout << "[GetPlotfiles] Warning could not parse filename \"" << filename << "\"\n";
            }
        }
        ++fs_iter;
    }
    return count;
}

void ShutdownAllThreads()
{
	stop_all_threads = true;
    stop_local_threads = true;

    for (std::vector<std::thread>::iterator it = worker_threads.begin() ; it != worker_threads.end(); ++it)
    {
        if(it->joinable())
        {
            it->join();
        }
    }
    if(updater_thread.joinable())
	{
		updater_thread.join();
	}
}

int EndMinerWithError()
{
    ShutdownAllThreads();
    std::cout << "Press enter to continue ...";
    std::cin.get();
    return 1;
}


// Helper routines taken from http://stackoverflow.com/questions/1557400/hex-to-char-array-in-c
int xdigit( char const digit )
{
    int val;
    if( '0' <= digit && digit <= '9' )
    {
        val = digit -'0';
    }
    else if( 'a' <= digit && digit <= 'f' )
    {
        val = digit -'a'+10;
    }
    else if( 'A' <= digit && digit <= 'F' )
    {
        val = digit -'A'+10;
    }
    else
    {
        val = -1;
    }
    return val;
}

bool HexStr2Str(std::string &out, const std::string in)
{
    if( !in.size() )
    {
        return false; // missing input string
    }

    size_t inlen = in.size();
    if( (inlen % 2) != 0 )
    {
        inlen--; // hex string must even sized
    }

    for ( char c : in)
    {
        if( xdigit(c) < 0 )
        {
            return false; // bad character in hex string
        }
    }

    out = "";
    for(size_t i=0; i<inlen; i+=2)
    {
        uint8_t both = xdigit(in[i])*16 + xdigit(in[i+1]);
        out.push_back(both);
    }

    out.push_back('\0');
    return true;
}

void PollLocal(void)
{
	const size_t buffer_size = 1000;   // |TODO remove this buffer
    char buffer[buffer_size] = {};

    int cmd_result = 0;
    struct addrinfo hints, *servinfo, *p;
	//SOCKET UpdaterSocket = INVALID_SOCKET;
    int sockfd;

	memset(&hints, 0, sizeof hints);
	hints.ai_family = AF_UNSPEC;       // also works with ipv6
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_protocol = IPPROTO_TCP;

	if (getaddrinfo(updater_address.c_str(), updater_port.c_str(), &hints, &servinfo) != 0)
    {
		if (network_quality > 0)
        {
            network_quality--;
        }
        // \TODO Show error of errno
		//Log("\n*! GMI: getaddrinfo failed with error: "); Log_u(WSAGetLastError());
	}
	else
    {
		sockfd = socket(servinfo->ai_family, servinfo->ai_socktype, servinfo->ai_protocol);
		if (sockfd == -1)
		{
			if (network_quality > 0)
            {
                network_quality--;
            }
            // \TODO Show error of errno
    		//Log("\n*! GMI: socket function failed with error: "); Log_u(WSAGetLastError());
		}
		else
        {
			const unsigned timeout = 1000;
			setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, (char *)&timeout, sizeof(timeout));
			//Log("\n*Connecting to server: "); Log(updateraddr); Log(":"); Log(updaterport);
			cmd_result = connect(sockfd, servinfo->ai_addr, (int)servinfo->ai_addrlen);
			if (cmd_result == -1)
            {
				if (network_quality > 0)
                {
                    network_quality--;
                }
                // \TODO Show error of errno
        		//Log("\n*! GMI: connect function failed with error: "); Log_u(WSAGetLastError());
			}
			else
            {
				int bytes = snprintf(buffer, buffer_size, "POST /burst?requestType=getMiningInfo HTTP/1.0\r\nHost: %s:%s\r\nContent-Length: 0\r\nConnection: close\r\n\r\n", server_address.c_str(), server_port.c_str());
				cmd_result = send(sockfd, buffer, bytes, 0);
				if (cmd_result == -1)
				{
					if (network_quality > 0)
                    {
                        network_quality--;
                    }
                    // \TODO Show error of errno
            		//Log("\n*! GMI: send request failed: "); Log_u(WSAGetLastError());
				}
				else
                {
                    memset(buffer, 0, buffer_size);
					size_t  pos = 0;
                    ssize_t res = 0;
					do
                    {
						res = recv(sockfd, &buffer[pos], (int)(buffer_size - pos - 1), 0);
						if (res > 0)
                        {
                            pos += res;
                        }
					} while (res > 0);
					if (res == -1)
					{
						if (network_quality)
                        {
                            network_quality--;
                        }
                        // \TODO Show error of errno
                		//Log("\n*! GMI: get mining info failed:: "); Log_u(WSAGetLastError());
					}
					else
                    {
						if (network_quality < 100)
                        {
                            network_quality++;
                        }
                        // \TODO Show in log
                		//Log("\n* GMI: Received: "); Log_server(buffer);

						// locate HTTP header
						char *find = strstr(buffer, "\r\n\r\n");
						if (find == nullptr)
                        {
                            // \TODO Show in log
                    		//Log("\n*! GMI: error message from pool");
                        }
						else
                        {
							//rapidjson::Document gmi;
                            nlohmann::json gmi;
                            try
                            {
                                 gmi = nlohmann::json::parse(find+4);
                            }
                            catch(std::invalid_argument e)
                            {
                                console::SetColor(console::color::FG_YELLOW);
                                std::cout << "[PollLocal] Could not parse answer: " << e.what() << std::endl;
                                console::SetColor(console::color::RESET);
                            }
                            //std::cout << std::setw(4) << gmi << std::endl;
							if (gmi.is_object())
							{
								if (!gmi["baseTarget"].empty())
                                {
									if (gmi["baseTarget"].is_string())
                                    {
                                        std::string tmp = gmi["baseTarget"];
                                        base_target = std::stoull(tmp);
                                    }
									else
                                    {
										if (gmi["baseTarget"].is_number())
                                        {
                                            base_target = gmi["baseTarget"];
                                        }
                                    }
								}

								if (!gmi["height"].empty())
                                {
									if (gmi["height"].is_string())
                                    {
                                        std::string tmp = gmi["height"];
                                        block_height = std::stoull(tmp);
                                    }
									else
                                    {
										if (gmi["height"].is_number())
                                        {
                                            block_height = gmi["height"];
                                        }
                                    }
								}

								if (!gmi["generationSignature"].empty())
                                {
                                    std::string tmp_signature = gmi["generationSignature"];
                                    if(signature_str.compare(tmp_signature) != 0)
                                    {
                                        std::cout << "[PollLocal] New signature!" << std::endl;
                                        signature_str = tmp_signature;
                                        std::lock_guard<std::mutex> lock(signature_mutex);
    									if (!HexStr2Str(signature, signature_str))
                                        {
                                            //Log("\n*! GMI: Node response: Error decoding generationsignature\n");
                                            console::SetColor(console::color::FG_YELLOW);
                                            std::cout << "[PollLocal] Could not convert signature from hex to number string" << std::endl;
                                            console::SetColor(console::color::RESET);
                                        }
                                    }
								}
								if (!gmi["targetDeadline"].empty())
                                {
									if (gmi["targetDeadline"].is_string())
                                    {
                                        std::string tmp = gmi["targetDeadline"];
                                        target_deadline_info = std::stoull(tmp);
                                    }
									else
                                    {
										if (gmi["targetDeadline"].is_number())
                                        {
                                            target_deadline_info = gmi["targetDeadline"];
                                        }
                                    }
								}
							}
						}
					}
				}
			}
			cmd_result = close(sockfd);
		}
		freeaddrinfo(servinfo);
	}
}

void updater_run()
{
    while (!stop_all_threads)
	{
        PollLocal();
		std::this_thread::yield();
        std::this_thread::sleep_for(std::chrono::milliseconds(update_interval));
    }
}

// helper functions
size_t GetIndexAcc( const uint64_t key)
{
    std::lock_guard<std::mutex> lock(bests_mutex);
	size_t acc_index = 0;
	for (auto it = bests.begin(); it != bests.end(); ++it)
	{
		if (it->account_id == key)
		{
			return acc_index;
		}
		acc_index++;
	}
	bests.push_back({key, 0, 0, 0, target_deadline_info});
	return bests.size() - 1;
}

void sender_run()
{
    // |TODO add to logger
    //Log("\nSender: started thread");
	//SOCKET ConnectSocket;
    int sockfd;

	int cmd_result = 0;
	const size_t buffer_size = 1000;   // |TODO remove this buffer
	char buffer[buffer_size] = {};     // |TODO maybe exchange this buffer for a string

    struct addrinfo hints, *servinfo, *p;

	while (!stop_all_threads && !stop_local_threads)
	{
		if (stop_local_threads == 1)
		{
			return;
		}

		for (auto share_it = shares.begin(); share_it != shares.end();)
		{
            //Гасим шару если она больше текущего targetDeadline, актуально для режима Proxy
            if ((share_it->best / base_target) > bests[GetIndexAcc(share_it->account_id)].target_deadline)
            {
                if (use_debug)
                {
                    std::time_t time_now_c = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
                    // |TODO make output dark red
                    std::cout << std::put_time(std::localtime(&time_now_c), "%T") << " [" << std::setw(20) << share_it->account_id << "]\t" << share_it->best / base_target << " > " << bests[GetIndexAcc(share_it->account_id)].target_deadline << " discarded" << std::endl;
                }
                {
                    std::lock_guard<std::mutex> lock(shares_mutex);
                    share_it = shares.erase(share_it);
                }
                continue;
            }

            memset(&hints, 0, sizeof hints);
            hints.ai_family = AF_UNSPEC;        // also works with ipv6
        	hints.ai_socktype = SOCK_STREAM;
        	hints.ai_protocol = IPPROTO_TCP;

            cmd_result = getaddrinfo(server_address.c_str(), server_port.c_str(), &hints, &servinfo);
            if (cmd_result != 0)
            {
                if (network_quality > 0)
                {
                    network_quality--;
                }
                console::SetColor(console::color::FG_RED);
                std::cout << "[Sender] getaddrinfo failed with error: " << std::strerror(errno) << std::endl;
                console::SetColor(console::color::RESET);
                continue;
            }
            //ConnectSocket = socket(result->ai_family, result->ai_socktype, result->ai_protocol);
            sockfd = socket(servinfo->ai_family, servinfo->ai_socktype, servinfo->ai_protocol);
            // |TODO maybe need to define INVALID_SOCKET as -1
            if (sockfd == INVALID_SOCKET)
            {
                if (network_quality > 0)
                {
                    network_quality--;
                }
                console::SetColor(console::color::FG_RED);
                std::cout << "[Sender] socket failed with error: " << std::strerror(errno) << std::endl;
                console::SetColor(console::color::RESET);
                freeaddrinfo(servinfo);
                continue;
            }
            const unsigned timeout = 1000;  // |TODO remove this
            //setsockopt(ConnectSocket, SOL_SOCKET, SO_RCVTIMEO, (char *)&timeout, sizeof(unsigned));
            setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, (char *)&timeout, sizeof(unsigned));
            //cmd_result = connect(ConnectSocket, result->ai_addr, (int)result->ai_addrlen);
            cmd_result = connect(sockfd, servinfo->ai_addr, (int)servinfo->ai_addrlen);

            if (cmd_result == SOCKET_ERROR)
            {
                if (network_quality > 0)
                {
                    network_quality--;
                }
                // |TODO add to logger
                //Log("\nSender:! Error Sender's connect: "); Log_u(WSAGetLastError());
                std::time_t time_now_c = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
                console::SetColor(console::color::FG_RED);
                std::cout << std::put_time(std::localtime(&time_now_c), "%T") << " [Sender] connect failed with error: " << std::strerror(errno) << std::endl;
                console::SetColor(console::color::RESET);
                freeaddrinfo(servinfo);
                continue;
            }
            else
            {
                freeaddrinfo(servinfo);

                int bytes = 0;
                //RtlSecureZeroMemory(buffer, buffer_size);
                memset(&buffer, 0, sizeof buffer);
                if (mode == MinerMode::solo)
                {
                    bytes = snprintf(buffer, buffer_size, "POST /burst?requestType=submitNonce&secretPhrase=%s&nonce=%llu HTTP/1.0\r\nHost: %s:%s\r\nConnection: close\r\n\r\n", passphrase.c_str(), share_it->nonce, server_address.c_str(), server_port.c_str());
                }
                if (mode == MinerMode::pool)
                {
                    uint64_t total = total_plotfile_size / 1024 / 1024 / 1024;
                    for (auto it = satellite_size.begin(); it != satellite_size.end(); ++it)
                    {
                        total = total + it->second;
                    }
                    bytes = snprintf(buffer, buffer_size, "POST /burst?requestType=submitNonce&accountId=%llu&nonce=%llu&deadline=%llu HTTP/1.0\r\nHost: %s:%s\r\nX-Miner: Blago %s\r\nX-Capacity: %llu\r\nContent-Length: 0\r\nConnection: close\r\n\r\n", share_it->account_id, share_it->nonce, share_it->best, server_address.c_str(), server_port.c_str(), version, total);
                }

                // Sending to server
                //cmd_result = send(ConnectSocket, buffer, bytes, 0);
                cmd_result = send(sockfd, buffer, bytes, 0);

                if (cmd_result == SOCKET_ERROR)
                {
                    if (network_quality > 0)
                    {
                        network_quality--;
                    }
                    // |TODO add to logger
                    //Log("\nSender: ! Error deadline's sending: "); Log_u(WSAGetLastError());
                    console::SetColor(console::color::FG_RED);
                    std::cout << "[Sender] send failed with error: " << std::strerror(errno) << std::endl;
                    console::SetColor(console::color::RESET);
                    continue;
                }
                else
                {
                    uint64_t dl = share_it->best / base_target;
                    std::time_t time_now_c = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
                    if (network_quality < 100)
                    {
                        network_quality++;
                    }
                    console::SetColor(console::color::FG_CYAN);
                    std::cout << std::put_time(std::localtime(&time_now_c), "%T") << " [" << std::setw(20) << share_it->account_id << "] sent DL: " \
                    << std::setw(15) << dl << " " << std::setw(5) << (dl) / (24 * 60 * 60) << "d " << std::setw(2) << \
                    (dl % (24 * 60 * 60)) / (60 * 60) << ":" << std::setw(2) << (dl % (60 * 60)) / 60 << ":" << std::setw(2) << dl % 60 << std::endl;
                    console::SetColor(console::color::RESET);
                    {
                        std::lock_guard<std::mutex> lock(sessions_mutex);
                        //sessions.push_back({ ConnectSocket, share_it->account_id, dl, share_it->best, share_it->nonce });
                        sessions.push_back(Session{ sockfd, dl, *share_it });
                    }
                    bests[GetIndexAcc(share_it->account_id)].target_deadline = dl;
                    {
                        std::lock_guard<std::mutex> lock(shares_mutex);
                        share_it = shares.erase(share_it);
                    }
                }
            }
        }

        if (!sessions.empty())
        {
            std::lock_guard<std::mutex> lock(sessions_mutex);
            for (auto session_it = sessions.begin(); session_it != sessions.end() && !stop_local_threads && !stop_all_threads;)
            {
                //ConnectSocket = session_it->Socket;
                sockfd = session_it->socket;

                //bool enable = TRUE;
                //cmd_result = ioctlsocket(ConnectSocket, FIONBIO, (unsigned long*)&enable);
                cmd_result = fcntl(sockfd, F_SETFL, O_NONBLOCK);
                if (cmd_result == SOCKET_ERROR)
                {
                    if (network_quality > 0)
                    {
                        network_quality--;
                    }
                    // |TODO add to logger
                    //Log("\nSender: ! Error ioctlsocket's: "); Log_u(WSAGetLastError());
                    console::SetColor(console::color::FG_RED);
                    std::cout << "[Sender] ioctlsocket failed with error: " << std::strerror(errno) << std::endl;
                    console::SetColor(console::color::RESET);
                    continue;
                }
                //RtlSecureZeroMemory(buffer, buffer_size);
                memset(&buffer, 0, sizeof buffer);
                size_t  pos = 0;
                ssize_t res = 0;
                do
                {
                    //res = recv(ConnectSocket, &buffer[pos], (int)(buffer_size - pos - 1), 0);
                    res = recv(sockfd, &buffer[pos], (int)(buffer_size - pos - 1), 0);
                    if (res > 0)
                    {
                        pos += res;
                    }
                } while (res > 0);

                if (res == SOCKET_ERROR)
                {
                    //if (WSAGetLastError() != WSAEWOULDBLOCK) // break connection, silently re-send deadline
                    if (errno != EWOULDBLOCK) // break connection, silently re-send deadline
                    {
                        if (network_quality > 0)
                        {
                            network_quality--;
                        }
                        //wattron(win_main, COLOR_PAIR(6));
                        //wprintw(win_main, "%s [%20llu] not confirmed DL %10llu\n", t_buffer, session_it->body.account_id, session_it->deadline, 0);
                        //wattroff(win_main, COLOR_PAIR(6));
                        // |TODO add to logger
                        //Log("\nSender: ! Error getting confirmation for DL: "); Log_llu(session_it->deadline);  Log("  code: "); Log_u(WSAGetLastError());
                        session_it = sessions.erase(session_it);
                        shares.push_back({ session_it->body.file_name, session_it->body.account_id, session_it->body.best, session_it->body.nonce });
                    }
                }
                else //что-то получили от сервера
                {
                    if (network_quality < 100)
                    {
                        network_quality++;
                    }

                    //получили пустую строку, переотправляем дедлайн
                    if (buffer[0] == '\0')
                    {
                        // |TODO add to logger
                        //Log("\nSender: zero-length message for DL: "); Log_llu(session_it->deadline);
                        shares.push_back({ session_it->body.file_name, session_it->body.account_id, session_it->body.best, session_it->body.nonce });
                    }
                    else //получили ответ пула
                    {
                        char *find = strstr(buffer, "{");
                        if (find == nullptr)
                        {
                            find = strstr(buffer, "\r\n\r\n");
                            if (find != nullptr)
                            {
                                find = find + 4;
                            }
                            else
                            {
                                find = buffer;
                            }
                        }

                        uint64_t n_deadline;
                        uint64_t n_account_id = 0;
                        uint64_t n_target_deadline = 0;
                        nlohmann::json answer;
                        try
                        {
                             answer = nlohmann::json::parse(find);
                        }
                        catch(std::invalid_argument e)
                        {
                            console::SetColor(console::color::FG_RED);
                            std::cout << "[sender_run] Could not parse answer: " << e.what() << std::endl;
                            console::SetColor(console::color::RESET);
                        }
                        // burst.ninja        {"requestProcessingTime":0,"result":"success","block":216280,"deadline":304917,"deadlineString":"3 days, 12 hours, 41 mins, 57 secs","targetDeadline":304917}
                        // pool.burst-team.us {"requestProcessingTime":0,"result":"success","block":227289,"deadline":867302,"deadlineString":"10 days, 55 mins, 2 secs","targetDeadline":867302}
                        // proxy              {"result": "proxy","accountId": 17930413153828766298,"deadline": 1192922,"targetDeadline": 197503}

                        if (answer.is_object())
                        {
                            if (!answer["deadline"].empty())
                            {
                                if (answer["deadline"].is_string())
                                {
                                    std::string tmp = answer["deadline"];
                                    n_deadline = std::stoull(tmp);
                                }
                                else
                                {
                                    if (answer["deadline"].is_number())
                                    {
                                        n_deadline = answer["deadline"];
                                    }
                                }
                                // |TODO add to logger
                                //Log("\nSender: confirmed deadline: "); Log_llu(ndeadline);

                                if (!answer["targetDeadline"].empty())
                                {
                                    if (answer["targetDeadline"].is_string())
                                    {
                                        std::string tmp = answer["targetDeadline"];
                                        n_target_deadline = std::stoull(tmp);
                                    }
                                    else
                                    {
                                        if (answer["targetDeadline"].is_number())
                                        {
                                            n_target_deadline = answer["targetDeadline"];
                                        }
                                    }
                                }
                                if (!answer["accountId"].empty())
                                {
                                    if (answer["accountId"].is_string())
                                    {
                                        std::string tmp = answer["accountId"];
                                        n_account_id = std::stoull(tmp);
                                    }
                                    else
                                    {
                                        if (answer["accountId"].is_number())
                                        {
                                            n_account_id = answer["accountId"];
                                        }
                                    }
                                }

                                uint64_t days = (n_deadline) / (24 * 60 * 60);
                                unsigned hours = (n_deadline % (24 * 60 * 60)) / (60 * 60);
                                unsigned min = (n_deadline % (60 * 60)) / 60;
                                unsigned sec = n_deadline % 60;

                                std::time_t time_now_c = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());

                                if ((n_account_id != 0) && (n_target_deadline != 0))
                                {
                                    {
                                        std::lock_guard<std::mutex> lock(bests_mutex);
                                        bests[GetIndexAcc(n_account_id)].target_deadline = n_target_deadline;
                                    }
                                    console::SetColor(console::color::FG_GREEN);
                                    std::cout << std::put_time(std::localtime(&time_now_c), "%T") << " [" << std::setw(20) << n_account_id << "] confirmed DL: " << std::setw(10) << n_deadline << " " \
                                    << std::setw(5) << days << "d " << std::setw(2) << hours << ":" << std::setw(2) << min << ":" << std::setw(2) << sec << std::endl;
                                    console::SetColor(console::color::RESET);
                                    if (use_debug)
                                    {
                                        // |TODO make output colorful
                                        std::cout << std::put_time(std::localtime(&time_now_c), "%T") << " [" << std::setw(20) << n_account_id << "] set DL: " << std::setw(10) << n_target_deadline << std::endl;
                                    }
                                }
                                else
                                {
                                    console::SetColor(console::color::FG_GREEN);
                                    std::cout << std::put_time(std::localtime(&time_now_c), "%T") << " [" << std::setw(20) << session_it->body.account_id << "] confirmed DL: " << std::setw(10) << n_deadline << " " \
                                    << std::setw(5) << days << "d " << std::setw(2) << hours << ":" << std::setw(2) << min << ":" << std::setw(2) << sec << std::endl;
                                    console::SetColor(console::color::RESET);
                                }
                                if (n_deadline < deadline || deadline == 0)
                                {
                                    deadline = n_deadline;
                                }

                                if (n_deadline != session_it->deadline)
                                {
                                    // |TODO make output colorful
                                    std::cout << "----Fast block or corrupted file?----\nSent deadline:\t" << session_it->deadline << "\nServer's deadline:\t" << n_deadline << "\n----" << std::endl;
                                }
                            }
                            else
                            {
                                if (!answer["errorDescription"].empty())
                                {
                                    //wattron(win_main, COLOR_PAIR(15));
                                    console::SetColor(console::color::FG_RED);
                                    std::cout << "[Sender] Error " << answer["errorCode"] << ": " << answer["errorDescription"] << std::endl;
                                    //wattron(win_main, COLOR_PAIR(12));
                                    if (answer["errorCode"] == 1004)
                                    {
                                        std::cout << "You need change reward assignment and wait 4 blocks (~16 minutes)" << std::endl; //error 1004
                                    }
                                    console::SetColor(console::color::RESET);
                                }
                                else
                                {
                                    //wattron(win_main, COLOR_PAIR(15));
                                    std::cout << find << std::endl;
                                }
                            }
                        }
                        else
                        {
                            if (strstr(find, "Received share") != nullptr)
                            {
                                std::time_t time_now_c = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
                                deadline = bests[GetIndexAcc(session_it->body.account_id)].deadline; //может лучше session_it->deadline ?
                                // if(deadline > session_it->deadline) deadline = session_it->deadline;
                                //wattron(win_main, COLOR_PAIR(10));
                                // |TODO make output green
                                std::cout << std::put_time(std::localtime(&time_now_c), "%T") << " [" << std::setw(20) << session_it->body.account_id << "] confirmed DL: " << std::setw(10) << session_it->deadline << std::endl;
                            }
                            else //получили нераспознанный ответ
                            {
                                int minor_version;
                                int status = 0;
                                const char *msg;
                                size_t msg_len;
                                struct phr_header headers[12];
                                size_t num_headers = sizeof(headers) / sizeof(headers[0]);
                                phr_parse_response(buffer, strlen(buffer), &minor_version, &status, &msg, &msg_len, headers, &num_headers, 0);

                                if (status != 0)
                                {
                                    //wattron(win_main, COLOR_PAIR(6));
                                    //wprintw(win_main, "%s [%20llu] NOT confirmed DL %10llu\n", tbuffer, session_it->body.account_id, session_it->deadline, 0);
                                    std::string error_str(msg, msg_len);
                                    console::SetColor(console::color::FG_RED);
                                    std::cout << "[Sender] Server error:" << status << " " << error_str << std::endl;
                                    console::SetColor(console::color::RESET);
                                    // |TODO add to logger
                                    //Log("\nSender: server error for DL: "); Log_llu(session_it->deadline);
                                    shares.push_back({ session_it->body.file_name, session_it->body.account_id, session_it->body.best, session_it->body.nonce });
                                }
                                else //получили непонятно что
                                {
                                    //wattron(win_main, COLOR_PAIR(7));
                                    std::cout << buffer << std::endl;
                                }
                            }
                        }
                    }
                    //cmd_result = closesocket(ConnectSocket);
                    cmd_result = close(sockfd);
                    // |TODO add to logger
                    //Log("\nSender: Close socket. Code = "); Log_u(WSAGetLastError());
                    session_it = sessions.erase(session_it);
                }
                if (session_it != sessions.end())
                {
                    ++session_it;
                }
            }
        }
        std::this_thread::yield();
        std::this_thread::sleep_for(std::chrono::milliseconds(send_interval));
    }
    return;
}

void procscoop_sph(const uint64_t nonce, const uint64_t n, char const *const data, const size_t acc, const std::string &file_name)
{
    char const *cache;
    char sig[32 + 64];
    cache = data;
    char res[32];
    //std::memcpy_s(sig, sizeof(sig), signature, sizeof(char) * 32);
    if(sizeof sig < sizeof(char) * 32)
    {
        console::SetColor(console::color::FG_YELLOW);
        std::cout << "[procscoop_sph] Warning buffer overflow!" << std::endl;
        console::SetColor(console::color::RESET);
    }
    {
        std::lock_guard<std::mutex> lock(signature_mutex);
        std::memcpy(sig, signature.c_str(), sizeof(char) * 32);
    }

    sph_shabal_context x, init_x;
    sph_shabal256_init(&init_x);
    for (uint64_t v = 0; v < n; v++)
    {
        if(sizeof(sig)-32 < sizeof(char) * 64)
        {
            console::SetColor(console::color::FG_YELLOW);
            std::cout << "[procscoop_sph] Warining buffer overflow!" << std::endl;
            console::SetColor(console::color::RESET);
        }
        //std::memcpy_s(&sig[32], sizeof(sig)-32, &cache[v * 64], sizeof(char)* 64);
        std::memcpy(&sig[32], &cache[v * 64], sizeof(char)* 64);

        std::memcpy(&x, &init_x, sizeof(init_x)); // optimization: sph_shabal256_init(&x);
        sph_shabal256(&x, (const unsigned char*)sig, 64 + 32);
        sph_shabal256_close(&x, res);

        uint64_t *wertung = (uint64_t*)res;

        if ((*wertung / base_target) <= bests[acc].target_deadline)
        {
            if (bests[acc].nonce == 0 || *wertung < bests[acc].best)
            {
                // |TODO add to logger
                //Log("\nfound deadline=");	Log_llu(*wertung / base_target); Log(" nonce=");	Log_llu(nonce + v); Log(" for account: "); Log_llu(bests[acc].account_id); Log(" file: "); Log((char*)file_name.c_str());
                {
                    std::lock_guard<std::mutex> lock(bests_mutex);
                    bests[acc].best = *wertung;
                    bests[acc].nonce = nonce + v;
                    bests[acc].deadline = *wertung / base_target;
                }
                {
                    std::lock_guard<std::mutex> lock(shares_mutex);
                    shares.push_back(Share{ file_name, bests[acc].account_id, bests[acc].best, bests[acc].nonce });
                }
                if (use_debug)
                {
                    std::time_t time_now_c = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
                    console::SetColor(console::color::FG_BLUE);
                    std::cout << std::put_time(std::localtime(&time_now_c), "%T") << " [" << std::setw(20) << bests[acc].account_id << "] found DL: " << std::setw(14) << bests[acc].deadline << std::endl;
                    console::SetColor(console::color::RESET);
                }
            }
        }
    }
}

void worker_run(const uint16_t worker_num, const std::string plot_path)
{
    std::chrono::system_clock::time_point start_work_time, end_work_time; //std::chrono::system_clock::now();
    std::chrono::system_clock::time_point start_time_read, end_time_read;
    std::chrono::system_clock::time_point start_time_proc;
    start_work_time = std::chrono::system_clock::now();

    /*if (use_boost)
    {
        //SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
        //SetThreadAffinityMask(GetCurrentThread(), 1 << (int)(working_threads));
        SetThreadIdealProcessor(GetCurrentThread(), (uint32_t)(worker_num % std::thread::hardware_concurrency()) );
    }*/
    uint64_t files_size_per_thread = 0;

    // |TODO add to logger
    // Log("\nStart thread: [");
    // Log_llu(worker_num);
    // Log("]  ");
    // Log((char*)plot_path.c_str());

    std::vector<Plotfile> plotfiles;
    GetPlotfiles(plot_path, plotfiles);

    size_t cache_size_local;
    uint32_t sectors_per_cluster;
    uint32_t bytes_per_sector;
    uint32_t number_of_free_clusters;
    uint32_t total_number_of_clusters;
    uint64_t sum_time_proc = 0;         // in nanosenconds \TODO ???

    for (Plotfile p : plotfiles)
    {
        uint64_t key, nonce, nonces, stagger, tail;
        start_time_read = std::chrono::system_clock::now();
        key = p.key;
        nonce = p.start_nonce;
        nonces = p.nonces;
        stagger = p.stagger;
        tail = 0;
        // Проверка кратности нонсов стаггеру
        if ((double)(nonces % stagger) > std::numeric_limits<double>::epsilon())
        {
            //wattron(win_main, COLOR_PAIR(12));
            console::SetColor(console::color::FG_YELLOW);
            std::cout << "[Worker] File \"" << p.name << "\" wrong stagger?" << std::endl;
            console::SetColor(console::color::RESET);
        }

        // Проверка на повреждения плота
        if (nonces != (p.size) / (4096 * 64))
        {
            //wattron(win_main, COLOR_PAIR(12));
            console::SetColor(console::color::FG_YELLOW);
            std::cout << "[Worker] File \"" << p.name << "\" name/size mismatch" << std::endl;
            console::SetColor(console::color::RESET);
            if (nonces != stagger)
            {
                nonces = (((p.size) / (4096 * 64)) / stagger) * stagger; //обрезаем плот по размеру и стаггеру
            }
            else if (scoop > (p.size) / (stagger * 64)) //если номер скупа попадает в поврежденный смерженный плот, то пропускаем
            {
                console::SetColor(console::color::FG_YELLOW);
                std::cout << "[Worker] skipped" << std::endl;
                console::SetColor(console::color::RESET);
                continue;
            }
        }

        //if (!GetDiskFreeSpaceA((p.path).c_str(), &sectors_per_cluster, &bytes_per_sector, &number_of_free_clusters, &total_number_of_clusters))
        if (!GetAvailableSpace((p.path).c_str(), sectors_per_cluster, bytes_per_sector, number_of_free_clusters, total_number_of_clusters))
        {
            console::SetColor(console::color::FG_YELLOW);
            std::cout << "[Worker] GetDiskFreeSpace failed" << std::endl;
            console::SetColor(console::color::RESET);
            continue;
        }

        // Если стаггер в плоте меньше чем размер сектора - пропускаем
        if ((stagger * 64) < bytes_per_sector)
        {
            console::SetColor(console::color::FG_YELLOW);
            std::cout << "[Worker] stagger (" << stagger << ") must be >= " << bytes_per_sector/64 << std::endl;
            console::SetColor(console::color::RESET);
            continue;
        }

        // Если нонсов в плоте меньше чем размер сектора - пропускаем
        if ((nonces * 64) < bytes_per_sector)
        {
            console::SetColor(console::color::FG_YELLOW);
            std::cout << "[Worker] nonces (" << nonces << ") must be >= " << bytes_per_sector/64 << std::endl;
            console::SetColor(console::color::RESET);
            continue;
        }

        // Если стаггер не выровнен по сектору - можем читать сдвигая последний стагер назад (доделать)
        if ((stagger % (bytes_per_sector/64)) != 0)
        {
            console::SetColor(console::color::FG_YELLOW);
            std::cout << "[Worker] stagger (" << stagger << ") must be a multiple of " << bytes_per_sector/64 << std::endl;
            console::SetColor(console::color::RESET);
            //uint64_t new_stagger = (stagger / (bytes_per_sector / 64)) * (bytes_per_sector / 64);
            //tail = stagger - new_stagger;
            //stagger = new_stagger;
            //nonces = (nonces/stagger) * stagger;
            //Нужно добавить остаток от предыдущего значения стаггера для компенсации сдвига по нонсам
            //wprintw(win_main, "stagger changed to %llu\n", stagger, 0);
            //wprintw(win_main, "nonces changed to %llu\n", nonces, 0);
            //continue;
        }


        if ((stagger == nonces) && (cache_size < stagger))
        {
            cache_size_local = cache_size;  // оптимизированный плот
        }
        else
        {
            cache_size_local = stagger; // обычный плот
        }

        // Выравниваем cache_size_local по размеру сектора
        cache_size_local = (cache_size_local / (size_t)(bytes_per_sector / 64)) * (size_t)(bytes_per_sector / 64);
        //wprintw(win_main, "round: %llu\n", cache_size_local);


        //char *cache = (char *)VirtualAlloc(nullptr, cache_size_local * 64, MEM_COMMIT, PAGE_READWRITE);
        char *cache = (char*)malloc(cache_size_local * 64);
        if (!cache)
        {
            EndMinerWithError();
            return;
        }
        memset(cache, 0, cache_size_local * 64);

        // |TODO add to logger
        //Log("\nRead file : ");	Log((char*)p.name.c_str());

        //wprintw(win_main, "%S \n", str2wstr(p.path + p.name).c_str());
        // HANDLE ifile = CreateFileA((p.path + p.name).c_str(), GENERIC_READ, FILE_SHARE_READ, 0, OPEN_EXISTING, FILE_FLAG_NO_BUFFERING, nullptr);
        //int fd = open((p.path + p.name).c_str(), O_RDWR | O_CREAT, 0666);
        std::ifstream file_stream{p.path + p.name, std::ios::in | std::ios::binary};
        if(!file_stream)
        {
            console::SetColor(console::color::FG_RED);
            std::cout << "[Worker] Error opening file \"" << p.name << "\": " << std::strerror(errno) << std::endl;
            console::SetColor(console::color::RESET);
            //VirtualFree(cache, 0, MEM_RELEASE);
            if(cache)
            {
                free(cache);
            }
            continue;
        }
        // file_stream.seekg (0, file_stream.end);
        // std::cout << "Size of file " << file_stream.tellg() << " Bytes" << std::endl;
        // file_stream.seekg (0, file_stream.beg);
        files_size_per_thread += p.size;

        uint64_t start, bytes;
        int32_t b = 0;
        //LARGE_INTEGER liDistanceToMove;
        uint64_t fd_offset = 0;

        size_t acc = GetIndexAcc(key);
        //std::cout << "[Worker] Start iterating" << std::endl;
        for (uint64_t n = 0; n < nonces; n += stagger)
        {
            //std::cout << "[Worker] Nonce " << n << std::endl;
            start = n * 4096 * 64 + scoop * stagger * 64;
            for (uint64_t i = 0; i < stagger; i += cache_size_local)
            {
                //std::cout << "[Worker] Stagger " << i << std::endl;
                if (i + cache_size_local > stagger)
                {
                    cache_size_local = stagger - i;  // остаток
                #ifdef __AVX2__
                    if (cache_size_local < 8)
                    {
                        console::SetColor(console::color::FG_YELLOW);
                        std::cout << "[Worker] Warning: " << cache_size_local << std::endl;
                        console::SetColor(console::color::RESET);
                    }
                #else
                #ifdef __AVX__
                    if (cache_size_local < 4)
                    {
                        console::SetColor(console::color::FG_YELLOW);
                        std::cout << "[Worker] Warning: " << cache_size_local << std::endl;
                        console::SetColor(console::color::RESET);
                    }
                #endif
                #endif
                }
                //liDistanceToMove.QuadPart = start + i*64;
                //if (!SetFilePointerEx(ifile, liDistanceToMove, nullptr, FILE_BEGIN))
                fd_offset = start + (i * 64);
                //std::cout << "[Worker] Using offset " << fd_offset << std::endl;
                //file_stream.clear();   // reset flags
                file_stream.seekg(fd_offset);
                if (!file_stream.good())
                {
                    console::SetColor(console::color::FG_RED);
                    std::cout << "[Worker] Error using seekg: " << std::strerror(errno) << std::endl;
                    console::SetColor(console::color::RESET);
                    if(cache)
                    {
                        free(cache);
                    }
                    continue;
                }
                // else
                // {
                //     std::cout << "Current pos " << file_stream.tellg() << std::endl;
                // }

                bytes = 0;
                b = 0;
                do
                {
                    //if (!ReadFile(ifile, &cache[bytes], (uint32_t)(cache_size_local * 64), &b, NULL))
                    //b = read(fd, &cache, (uint32_t)(cache_size_local * 64));
                    file_stream.read(cache, cache_size_local * 64);
                    if (!file_stream.good())
                    {
                        console::SetColor(console::color::FG_RED);
                        std::cout << "[Worker] Error reading file: " << strerror(errno) << std::endl;
                        console::SetColor(console::color::RESET);
                        break;
                    }
                    else
                    {
                        //std::cout << "read " << file_stream.gcount() << " Bytes" << std::endl;
                        bytes += file_stream.gcount();
                         //bytes = (cache_size_local * 64);
                    //     std::cout << "[Worker] Hure: " << std::string(cache, cache_size_local) << std::endl;
                    }
                    //wprintw(win_main, "%llu   %llu\n", bytes, readsize);
                } while (file_stream && bytes < (cache_size_local * 64) && !stop_all_threads);

                //std::cout << "[Worker] Read file" << std::endl;

                if (bytes == cache_size_local * 64)
                {
                    start_time_proc = std::chrono::system_clock::now();
                #ifdef __AVX2__
                    procscoop_m256_8(n + nonce + i, cache_size_local, cache, acc, p.name);  // Process block AVX2
                #else
                #ifdef __AVX__
                    procscoop_m_4(n + nonce + i, cache_size_local, cache, acc, p.name);     // Process block AVX
                #else
                    procscoop_sph(n + nonce + i, cache_size_local, cache, acc, p.name);     // Process block SSE4
                #endif
                #endif
                    sum_time_proc += std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now() - start_time_proc).count();
                    worker_progress[worker_num].reads_bytes += bytes;
                }
                else
                {
                    console::SetColor(console::color::FG_YELLOW);
                    std::cout << "[Worker] Unexpected end of file \"" << p.name << "\"" << std::endl;
                    console::SetColor(console::color::RESET);
                    break;
                }

                if (stop_local_threads || stop_all_threads) // New block while processing: Stop.
                {
                    worker_progress[worker_num].is_alive = false;
                    // |TODO add to logger
                    //Log("\nReading file: ");	Log((char*)p.name.c_str()); Log(" interrupted");
                    //CloseHandle(ifile);
                    file_stream.close();
                    plotfiles.clear();
                    //VirtualFree(cache, 0, MEM_RELEASE);
                    if(cache)
                    {
                        free(cache);
                    }
                    //if (use_boost) SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_NORMAL);
                    return;
                }
            }
        }
        end_time_read = std::chrono::system_clock::now();
        //Log("\nClose file: ");
        // Log((char*)p.name.c_str());
        // Log(" [@ ");
        // Log_llu((long long unsigned)((double)(end_time_read - start_time_read) * 1000 / pcFreq)); Log(" ms]");
        //CloseHandle(ifile);
        file_stream.close();
        if(cache)
        {
            free(cache);
        }
    }
    worker_progress[worker_num].is_alive = false;
    //QueryPerformanceCounter((LARGE_INTEGER*)&end_work_time);
    end_work_time = std::chrono::system_clock::now();

    //if (use_boost) SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_NORMAL);

    //double thread_time = (double)(end_work_time - start_work_time) / pcFreq;
    uint64_t milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(end_work_time - start_work_time).count();
    if (use_debug)
    {
        //wattron(win_main, COLOR_PAIR(7));
        std::time_t time_now_c = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
        // |TODO make output white
        // |TODO FIX THIS ERROR
        double thread_speed = 0;
        double cpu_usage = 0;
        if(files_size_per_thread && milliseconds)
        {
            thread_speed = double(files_size_per_thread) / milliseconds / 1024 / 1024 / 4096;
        }
        if(sum_time_proc && milliseconds)
        {
            cpu_usage = double(sum_time_proc) / milliseconds / 1000 / 1000;
        }
        //std::cout << "milliseconds " << milliseconds << ", thread_speed " << thread_speed << ", cpu_usage " << cpu_usage << std::endl;
        std::cout << std::put_time(std::localtime(&time_now_c), "%T") << " Thread \"" << plot_path << "\"";
        std::cout << " @ " << milliseconds / 1000.0 << " s (" << std::fixed << thread_speed << " MiB/s)";
        std::cout << " CPU " << std::fixed << cpu_usage << " %" << std::endl;
    }
    return;
}

struct sigaction sig_int_handler;

void SignalHandler(int signal)
{
    // end program
    std::cout << std::endl;
    std::cout << "Shutting down miner!" << std::endl;
    ShutdownAllThreads();
    std::cout << "Shutdown complete!" << std::endl;
    exit(0);
}

void SetupSignalHandler()
{
    sig_int_handler.sa_handler = SignalHandler;
    sigfillset(&sig_int_handler.sa_mask);
    sig_int_handler.sa_flags = 0;

    sigaction(SIGINT, &sig_int_handler, NULL);
    //sigaction(SIGSEGV, &sig_int_handler, NULL);
    sigaction(SIGILL, &sig_int_handler, NULL);
    sigaction(SIGFPE, &sig_int_handler, NULL);
    sigaction(SIGABRT, &sig_int_handler, NULL);
    sigaction(SIGTERM, &sig_int_handler, NULL);
    stop_all_threads = false;
}

bool GetJSON(const std::string req,  nlohmann::json &ret_json)
{
    std::string buffer;

	uint64_t msg_len = 0;
	int cmd_result = 0;
	struct addrinfo hints, *servinfo, *p;
	int wallet_socket_fd;
    bool ret_val = false;

	memset(&hints, 0, sizeof hints);
	//hints.ai_family = AF_UNSPEC;       // also works fo ipv6
	hints.ai_family = AF_INET;       // also works fo ipv6
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_protocol = IPPROTO_TCP;

	cmd_result = getaddrinfo( info_address.c_str(), info_port.c_str(), &hints, &servinfo);
	if (cmd_result != 0)
    {
		console::SetColor(console::color::FG_RED);
        std::cout << "[GetJSON] WINNER: Getaddrinfo failed with error: " << std::strerror(errno) << std::endl;
        console::SetColor(console::color::RESET);
        // |TODO add to logger
		//Log("\nWinner: getaddrinfo error");
	}
	else
	{
		wallet_socket_fd = socket(servinfo->ai_family, servinfo->ai_socktype, servinfo->ai_protocol);
		if (wallet_socket_fd == INVALID_SOCKET)
		{
			console::SetColor(console::color::FG_RED);
            std::cout << "[GetJSON] Socket function failed with error: " << std::strerror(errno) << std::endl;
            console::SetColor(console::color::RESET);
            // |TODO add to logger
    		//Log("\nWinner: Socket error: "); Log_u(WSAGetLastError());
		}
		else
		{
			const unsigned timeout = 3000;
			setsockopt(wallet_socket_fd, SOL_SOCKET, SO_RCVTIMEO, (char *)&timeout, sizeof(timeout));
			cmd_result = connect(wallet_socket_fd, servinfo->ai_addr, (int)servinfo->ai_addrlen);
			if (cmd_result == SOCKET_ERROR)
            {
				console::SetColor(console::color::FG_RED);
                std::cout << "[GetJSON] WINNER: Connect function failed with error: " << std::strerror(errno) << std::endl;
                console::SetColor(console::color::RESET);
                // |TODO add to logger
        		//Log("\nWinner: Connect server error "); Log_u(WSAGetLastError());
			}
			else
			{
				cmd_result = send(wallet_socket_fd, req.c_str(), (int)strlen(req.c_str()), 0);
				if (cmd_result == SOCKET_ERROR)
				{
					console::SetColor(console::color::FG_RED);
                    std::cout << "[GetJSON] WINNER: Send request failed: " << std::strerror(errno) << std::endl;
                    console::SetColor(console::color::RESET);
                    // |TODO add to logger
            		//Log("\nWinner: Error sending request: "); Log_u(WSAGetLastError());
				}
				else
				{
                    char tmp_buffer[2048] = {};
					ssize_t received_size = 0;
					while ((received_size = recv(wallet_socket_fd, &tmp_buffer, 2048, 0)) > 0)
					{
                        // |TODO add to logger
						//Log("\nrealloc: ");
                        buffer.append(tmp_buffer, received_size);
                        // |TODO add to logger
						//Log_llu(msg_len);
					}

					if (received_size < 0)
					{
						console::SetColor(console::color::FG_RED);
                        std::cout << "[GetJSON] WINNER: Get info failed: " << std::strerror(errno) << std::endl;
                        console::SetColor(console::color::RESET);
                        // |TODO add to logger
                		//Log("\nWinner: Error response: "); Log_u(WSAGetLastError());
					}
					else
					{
                        std::size_t found = buffer.find("\r\n\r\n");
                        if (found != std::string::npos)
						{
                            try
                            {
                                ret_json = nlohmann::json::parse(buffer.substr(found+4));
                                ret_val = true;
                            }
                            catch(std::invalid_argument e)
                            {
                                console::SetColor(console::color::FG_YELLOW);
                                std::cout << "[GetJSON] Could not parse answer of request \"" << req << "\". Error: " << e.what() << std::endl;
                                console::SetColor(console::color::RESET);
                                //std::cout << "[GetJSON] Answer was: " << buffer << std::endl;
                            }
						}
					} // recv() != SOCKET_ERROR
				} //send() != SOCKET_ERROR
			} // Connect() != SOCKET_ERROR
		} // socket() != INVALID_SOCKET
		cmd_result = close(wallet_socket_fd);
	} // getaddrinfo() == 0
	freeaddrinfo(servinfo);
	return ret_val;
}

void GetBlockInfo(unsigned const num_block)
{
	std::string generator;
	std::string generator_rs;
	unsigned long long last_block_height = 0;
	std::string name;
	std::string reward_recipient;
	std::string pool_account_rs;
	std::string pool_name;
	uint64_t timestamp0 = 0;
	uint64_t timestamp1 = 0;
	nlohmann::json doc_block;
	// Request the last two blocks from the blockchain

	std::string str_req;
    const size_t buffer_size = 1000;   // |TODO remove this buffer
    char buffer[buffer_size] = {};
    str_req = "POST /burst?requestType=getBlocks&firstIndex=" + std::to_string(num_block) + "&lastIndex=" + std::to_string(num_block+1) + " HTTP/1.0\r\nHost: " + info_address + ":" + info_port + "\r\nConnection: close\r\n\r\n";
	//Log("\n getBlocks: ");

    if(GetJSON(str_req, doc_block))
    {
    	if (!doc_block["blocks"].empty())
    	{
    		nlohmann::json blocks = doc_block["blocks"];
    		if (blocks.is_array())
    		{
    			const nlohmann::json& bl_0 = blocks[0];
    			const nlohmann::json& bl_1 = blocks[1];
                generator_rs = bl_0["generatorRS"];
                generator = bl_0["generator"];
    			last_block_height = bl_0["height"];
    			timestamp0 = bl_0["timestamp"];
    			timestamp1 = bl_1["timestamp"];
    		}
    	}
    	else
        {
            // |TODO add to logger
            //Log("\n- error parsing JSON getBlocks");
        }
    }
    else
    {
        console::SetColor(console::color::FG_YELLOW);
        std::cout << "[GetBlockInfo] Error could not get blocks!" << std::endl;
        console::SetColor(console::color::RESET);
    }

	if (!generator.empty() && !generator_rs.empty() && (timestamp0 != 0) && (timestamp1 != 0))
    {
		if (last_block_height == block_height - 1)
		{
			// request account information
			str_req.clear();
            str_req = "POST /burst?requestType=getAccount&account=" + generator + " HTTP/1.0\r\nHost: " + info_address + ":" + info_port + "\r\nConnection: close\r\n\r\n";
			//Log("\n getAccount: ");

            if(GetJSON(str_req, doc_block))
            {
    			if (doc_block["name"].empty())
                {
                    // |TODO add to logger
                    //Log("\n- error in message from pool (getAccount)\n");
                }
    			else
    			{
    				name = doc_block["name"];
    			}
            }
            else
            {
                console::SetColor(console::color::FG_YELLOW);
                std::cout << "[GetBlockInfo] Error could not get account information!" << std::endl;
                console::SetColor(console::color::RESET);
            }
			str_req.clear();
            str_req = "POST /burst?requestType=getRewardRecipient&account=" + generator + " HTTP/1.0\r\nHost: " + info_address + ":" + info_port + "\r\nConnection: close\r\n\r\n";
			//Log("\n getRewardRecipient: ");
            if(GetJSON(str_req, doc_block))
			{
                if (doc_block["rewardRecipient"].empty())
                {
                    // |TODO add to logger
                    //Log("\n- error in message from pool (getRewardRecipient)\n");
                }
    			else
    			{
    				reward_recipient = doc_block["rewardRecipient"];
    			}
            }
            else
            {
                console::SetColor(console::color::FG_YELLOW);
                std::cout << "[GetBlockInfo] Error could not get reward recipient!" << std::endl;
                console::SetColor(console::color::RESET);
            }

			if (!reward_recipient.empty())
			{
				// If reward_recipient! = Generator, then minitize to the pool, we learn the name of the pool ...
				if (generator.compare(reward_recipient) != 0)
				{
					// request pool account data
                    str_req.clear();
					str_req = "POST /burst?requestType=getAccount&account=" + reward_recipient + " HTTP/1.0\r\nHost: " + info_address + ":" + info_port + "\r\nConnection: close\r\n\r\n";
					//Log("\n getAccount: ");

                    if(GetJSON(str_req, doc_block))
                    {
    					if (doc_block["accountRS"].empty())
    					{
                            // |TODO add to logger
                            //Log("\n- error in message from pool (pool getAccount)\n");
    					}
    					else
    					{
    						pool_account_rs = doc_block["accountRS"];
    						if (!doc_block["name"].empty())
    						{
    							pool_name = doc_block["name"];
    						}
    					}
                    }
                    else
                    {
                        console::SetColor(console::color::FG_YELLOW);
                        std::cout << "[GetBlockInfo] Error could not get account data!" << std::endl;
                        console::SetColor(console::color::RESET);
                    }
				}
			}

			//wattron(win_main, COLOR_PAIR(11));
			std::time_t time_now_c = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
			if (!name.empty())
            {
                std::cout << std::put_time(std::localtime(&time_now_c), "%T") << " Winner: " << timestamp0 - timestamp1 << "s by " << generator_rs.substr(6) << " (" << name << ")" << std::endl;
            }
			else
            {
                std::cout << std::put_time(std::localtime(&time_now_c), "%T") << " Winner: " << timestamp0 - timestamp1 << "s by " << generator_rs.substr(6) << std::endl;
            }
			if (!pool_account_rs.empty())
			{
				if (!pool_name.empty())
                {
                    std::cout << std::put_time(std::localtime(&time_now_c), "%T") << " Winner's pool: " << pool_account_rs.substr(6) << " (" << pool_name << ")" << std::endl;
                }
				else
                {
                    std::cout << std::put_time(std::localtime(&time_now_c), "%T") << " Winner's pool: " << pool_account_rs.substr(6) << std::endl;
                }
			}
		}
		else
		{
			//wattron(win_main, COLOR_PAIR(11));
            std::time_t time_now_c = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
            std::cout << std::put_time(std::localtime(&time_now_c), "%T") << " Winner: no info yet" << std::endl;
		}
    }
}

int main(int argc, char *argv[])
{
    SetupSignalHandler();
    console::SetColor(console::color::FG_MAGENTA);
    std::cout << std::endl;
    std::cout << "Started miner++ by enwi(BURST-EUT4-AZ5X-L6SR-AXTNC)" << std::endl;
    std::cout << "If you like this miner please consider donating to above mentioned wallet, thx!" << std::endl;
    std::cout << "-------------------------------------------------------------------------------" << std::endl << std::endl;
    console::SetColor(console::color::RESET);

    mode = MinerMode::unknown;
    if (!LoadConfig(GetExePath() + "/conf.json"))
    {
        //std::cout << "[main] Error could not load config file!\n";
        return EndMinerWithError();
    }

    // print options \TODO move to extra function to keep main clean
    std::cout << std::endl;
    std::cout << "Mode:\t" << MinerModeToStr(mode) << std::endl;
    std::cout << "Server:\t" << server_address << ":" << server_port << std::endl;
    std::cout << "Paths:\t";
    for(std::string path : plot_paths)
    {
        std::cout << "\"" << path << "\", ";
    }
    std::cout << std::endl;


    // read passphrase if mode is solo miner
    if (mode == MinerMode::solo)
    {
        if (!ReadPassphrase(GetExePath() + "/passphrase.txt"))
        {
            return EndMinerWithError();
        }
    }

    // resolve all needed hostnames
    std::cout << "\n";
    server_ip = HostnameToIP(server_address);
    std::cout << "Pool address " << server_address << " resolved to " << server_ip << std::endl;

    if(updater_address.compare(server_address))
    {
        updater_ip = server_ip;
    }
    else
    {
        updater_ip = HostnameToIP(updater_address);
        std::cout << "Updater address " << updater_address << " resolved to " << updater_ip << std::endl;
    }

    // get all plotfiles
    std::vector<Plotfile> all_plotfiles;
    for(std::string path : plot_paths)
    {
        std::vector<Plotfile> dir_plotfiles;
        GetPlotfiles(path, dir_plotfiles);

        uint64_t total_size = 0;
        for(Plotfile& pf : dir_plotfiles)
        {
            total_size += pf.size;
            all_plotfiles.push_back(pf);
        }
        std::cout << std::fixed << path << ":\t" << dir_plotfiles.size() << " plotfiles, " << total_size / 1024.0 / 1024.0 / 1024.0 << " GiB" << std::endl;
        total_plotfile_size += total_size;
    }

    std::cout << std::fixed << "TOTAL: " << all_plotfiles.size() << " plotfiles, " << total_plotfile_size / 1024.0 / 1024.0 / 1024.0 << " GiB" << std::endl;

    if(total_plotfile_size == 0)
    {
        console::SetColor(console::color::FG_RED);
        std::cout << "[main] Error no plotfiles found!" << std::endl;
        console::SetColor(console::color::RESET);
        return EndMinerWithError();
    }

    // check for overlapped plots
	for (size_t cx = 0; cx < all_plotfiles.size(); ++cx)
    {
        Plotfile pf1 = all_plotfiles[cx];
		for (size_t cy = cx + 1; cy < all_plotfiles.size(); ++cy)
        {
            Plotfile pf2 = all_plotfiles[cy];
			if (pf2.key == pf1.key)
            {
				if (pf2.start_nonce >= pf1.start_nonce)
                {
					if (pf2.start_nonce < pf1.start_nonce + pf1.nonces)
                    {
                        console::SetColor(console::color::FG_YELLOW);
                        std::cout << "[main] Warning \"" << pf1.path << "\" and \"" << pf2.path << "\" are overlapped!" << std::endl;
                        console::SetColor(console::color::RESET);
					}
				}
				else
                {
					if (pf2.start_nonce + pf2.nonces > pf1.start_nonce)
                    {
                        console::SetColor(console::color::FG_YELLOW);
						std::cout << "[main] Warning \"" << pf1.path << "\" and \"" << pf2.path << "\" are overlapped!" << std::endl;
                        console::SetColor(console::color::RESET);
					}
                }
            }
		}
    }

    // start proxy thread \TODO add this stuff
    // std::thread proxy;
	// if (enable_proxy)
	// {
	// 	proxy = std::thread(proxy_i);
	// 	wattron(win_main, COLOR_PAIR(25));
	// 	wprintw(win_main, "Proxy thread started\n", 0);
	// 	wattroff(win_main, COLOR_PAIR(25));
    // }

    // start updater thread
	updater_thread = std::thread(updater_run);

    // \TODO Show in log
    //Log("\nUpdate mining info");
	while (block_height == 0 && !stop_all_threads)
	{
		std::this_thread::yield();
		std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    // variable setup
    std::chrono::system_clock::time_point time_now, end_threads_time, last_print_time;

    // main loop
    while (!stop_all_threads)
    {
        worker_threads.clear();
        worker_progress.clear();
        stop_local_threads = false;


        char scoopgen[40];
        {
            std::lock_guard<std::mutex> lock(signature_mutex);
            memmove(scoopgen, signature.c_str(), 32);
        }
		const char *mov = (char*)&block_height;
        scoopgen[32] = mov[7]; scoopgen[33] = mov[6]; scoopgen[34] = mov[5]; scoopgen[35] = mov[4]; scoopgen[36] = mov[3]; scoopgen[37] = mov[2]; scoopgen[38] = mov[1]; scoopgen[39] = mov[0];

        sph_shabal_context ctx;
		sph_shabal256_init(&ctx);
		sph_shabal256(&ctx, (const unsigned char*)(const unsigned char*)scoopgen, 40);
		char ctx_cache[32];
        sph_shabal256_close(&ctx, ctx_cache);

		scoop = (((unsigned char)ctx_cache[31]) + 256 * (unsigned char)ctx_cache[30]) % 4096;

        deadline = 0;

        std::cout << std::endl << std::endl << "------------------------------------------------------------" << std::endl << std::endl;

        time_now = std::chrono::system_clock::now();
        std::time_t time_now_c = std::chrono::system_clock::to_time_t(time_now);
        std::cout << std::put_time(std::localtime(&time_now_c), "%T") << " New block " << block_height << ", baseTarget " << base_target << ", netDiff " << 4398046511104 / 240 / base_target << " TiB" << std::endl;

        if (mode == MinerMode::solo)
        {
            uint64_t sat_total_size = 0;
            // \TODO clean this up
			for (auto it = satellite_size.begin(); it != satellite_size.end(); ++it)
            {
                sat_total_size += it->second;
            }
            std::cout << "*** Chance to find a block: " << ((double)((sat_total_size * 1024 + total_plotfile_size / 1024 / 1024) * 100 * 60)*(double)base_target) / 1152921504606846976 << " (" << sat_total_size + total_plotfile_size / 1024 / 1024 / 1024 << " GiB)" << std::endl;
        }

        {
            std::lock_guard<std::mutex> lock(sessions_mutex);
    		for (Session s : sessions)
            {
#ifdef __WIN32
                closesocket(s.socket);
#elif defined(__linux__) || defined(__APPLE__)
                close(s.socket);
#endif
            }
            sessions.clear();
        }

        {
            std::lock_guard<std::mutex> lock(shares_mutex);
    		shares.clear();
        }

		{
            std::lock_guard<std::mutex> lock(bests_mutex);
    		bests.clear();
            bests_mutex.unlock();
        }

        if ((target_deadline_info > 0) && (target_deadline_info < my_target_deadline))
        {
            // |TODO add to logger
			//Log("\nUpdate targetDeadline: ");
            //Log_llu(target_deadline_info);
		}
		else
        {
            target_deadline_info = my_target_deadline;
        }

        // start sender thread
		std::thread sender_thread(sender_run);

        // run threads
		std::chrono::system_clock::time_point start_threads_time = std::chrono::system_clock::now();
        double threads_speed = 0;

        for (size_t i = 0; i < plot_paths.size(); i++)
		{
			worker_progress.push_back({ i, 0, true });
			worker_threads.push_back(std::thread(worker_run, i, plot_paths[i]));
        }

        {
            std::lock_guard<std::mutex> lock(signature_mutex);
            old_signature = signature;
        }
		uint64_t old_baseTarget = base_target;
		uint64_t old_height = block_height;


        uint8_t last_percentage = 0;
        // Wait until signature changed or exit
        bool new_signature = false;
		while ( !new_signature && !stop_all_threads)
		{
            {
                std::lock_guard<std::mutex> lock(signature_mutex);
                new_signature = (signature.compare(old_signature) != 0);
            }
			/*switch (wgetch(win_main))
			{
			case 'q':
				exit_flag = true;
				break;
			case 'r':
				wattron(win_main, COLOR_PAIR(15));
				wprintw(win_main, "Recommended size for this block: %llu Gb\n", (4398046511104 / baseTarget)*1024 / targetDeadlineInfo);
				wattroff(win_main, COLOR_PAIR(15));
				break;
			case 'c':
				wprintw(win_main, "*** Chance to find a block: %.5f%%  (%llu Gb)\n", ((double)((total_size / 1024 / 1024) * 100 * 60)*(double)baseTarget) / 1152921504606846976, total_size / 1024 / 1024 / 1024, 0);
				break;
			}*/
			//box(win_progress, 0, 0);
			uint64_t bytes_read = 0;

			uint16_t threads_runing = 0;
			for (auto it = worker_progress.begin(); it != worker_progress.end(); ++it)
			{
				bytes_read += it->reads_bytes;
				threads_runing += it->is_alive;
			}

			if (threads_runing)
			{
                end_threads_time = std::chrono::system_clock::now();
				threads_speed = (double)(bytes_read / (1024 * 1024)) / std::chrono::duration_cast<std::chrono::seconds>(end_threads_time - start_threads_time).count();
			}
			else
            {
				/*
				if (can_generate == 1)
				{
					Log("\nStart Generator. ");
					for (size_t i = 0; i < std::thread::hardware_concurrency()-1; i++)	generator.push_back(std::thread(generator_i, i));
					can_generate = 2;
				}
				*/
				if (use_hdd_wakeup)
				{
                    time_now = std::chrono::system_clock::now();
					if (std::chrono::duration_cast<std::chrono::seconds>(time_now - end_threads_time).count() > 180)  // 3 minutes
					{
						std::vector<Plotfile> tmp_files;
                        for(std::string path : plot_paths)
                        {
                            GetPlotfiles(path, tmp_files);
                        }
						if (use_debug)
						{
                            std::time_t time_now_c = std::chrono::system_clock::to_time_t(time_now);
							//wattron(win_main, COLOR_PAIR(7));
                            std::cout << std::put_time(std::localtime(&time_now_c), "%T") << " HDD, WAKE UP !" << std::endl;
						}
						end_threads_time = time_now;
					}
				}
			}

			//wmove(win_progress, 1, 1);
			//wattron(win_progress, COLOR_PAIR(14));
            uint8_t percentage = (bytes_read * 4096 * 100 / total_plotfile_size);
            if (percentage != last_percentage)
            {
                last_percentage = percentage;
                uint64_t total_gib = (bytes_read / (256 * 1024));
    			if (deadline == 0)
                {
    				//wprintw(win_progress, "%3llu%% %6llu GB (%.2f MB/s). no deadline            Connection: %3u%%", percentage, total_gib, threads_speed, network_quality, 0);
                    std::cout << "Progress " << std::setw(3) << unsigned(percentage) << "% " << std::setw(6) << total_gib << " GiB (" << std::fixed << std::setw(6) << std::setprecision(2) << threads_speed << " MiB/s). no deadline            Connection: " << std::setw(3) << unsigned(network_quality) << "%" << std::endl;
                }
    			else
                {
    				//wprintw(win_progress, "%3llu%% %6llu GB (%.2f MB/s). Deadline =%10llu   Connection: %3u%%", percentage, total_gib, threads_speed, deadline, network_quality, 0);
                    std::cout << "Progress " << std::setw(3) << unsigned(percentage) << "% " << std::setw(6) << total_gib << " GiB (" << std::fixed << std::setw(6) << std::setprecision(2) << threads_speed << " MiB/s). deadline =" << std::setw(10) << deadline << "   Connection: " << std::setw(3) << unsigned(network_quality) << "%" << std::endl;
                }
            }

			std::this_thread::yield();
			std::this_thread::sleep_for(std::chrono::milliseconds(39));
        }

        stop_local_threads = true;

        if (show_winner && !stop_all_threads)
        {
            GetBlockInfo(0);
        }


        for (std::vector<std::thread>::iterator it = worker_threads.begin() ; it != worker_threads.end(); ++it)
        {
            if(it->joinable())
            {
                it->join();
            }
        }

        if (sender_thread.joinable())
        {
            sender_thread.join();
        }

        // fopen_s(&pFileStat, "stat.csv", "a+t");
		// if (pFileStat != nullptr)
		// {
		// 	fprintf(pFileStat, "%llu;%llu;%llu\n", old_height, old_baseTarget, deadline);
		// 	fclose(pFileStat);
        // }

    }

    // end program
    std::cout << "End" << std::endl;
    ShutdownAllThreads();
    return 0;
}

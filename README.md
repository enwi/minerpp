# minerpp
simple burstcoin miner for linux(even arm)
 * Ported from [miner-burst](https://github.com/Blagodarenko/miner-burst)
 * Tested on MacOS and Debian Wheezy.
 * Uses the same config file as miner-burst

Note that CPU usage of the mining threads as well as speed (MiB/s) do not represent the correct value.

## Build
### 1. Create new folder for binaries
After cloning this repo change to it's folder:
```
cd minerpp
```
Then create a new folder named \"bin\":
```
mkdir bin
```
Change to the newly created folder:
```
cd bin
```
### 2. Generate Makefile using CMake
Before we can compile the source we need to generate our Makefile.
For that we use CMake. If you don't have it, install it first (e.g. `sudo apt-get install cmake`).
Then execute the CMake command:
```
cmake ..
```
Since we need `std::filesystem` support, gcc/g++ 5 is the minimum required compiler version for this to work.
If you don't have it (check with `gcc --version`) you will need to install a newer version before using CMake.

For SBC's (Single Board Computer) use this [link](https://solarianprogrammer.com/2017/12/08/raspberry-pi-raspbian-install-gcc-compile-cpp-17-programs/)
to get gcc/g++ 7.2.0. It will work on all common distros of linux for SBC's (was tested on Odroid HC1 with
DietPi and Raspberry Pi 3+ with Raspbian Wheezy). On newer distro versions (e.g. Stretch) just install
gcc-5 from your prefered package manager (e.g. `sudo apt-get install gcc-5`).

On MacOS the standard `Clang` compiler will not work! Here you will also need to install gcc manually.
For that use the following command:
```
brew install gcc@7
```

If you successfully installed a newer compiler version you can run CMake with the following options:
```
cmake -DCMAKE_C_COMPILER=<path/to/gcc> -DCMAKE_CXX_COMPILER=<path/to/g++> ..
```
Replace `<path/to/gcc>` with the path to your gcc installation (e.g. SBC: `/usr/local/gcc-7.2.0/bin/gcc-7.2.0`, MacOS `/usr/local/bin/gcc-7`)
and `<path/to/g++>` with the path to your g++ installation (e.g. SBC: `/usr/local/gcc-7.2.0/bin/g++-7.2.0`, MacOS: `/usr/local/bin/g++-7`).
### 3. Compile the source code
To compile the source code simply run make:
```
make -j<number of threads>
```
Replace `<number of threads>` with the number of threads you want to use to compile to speedup the process.
### 4. Run the miner
Now you can just run the miner with the following command:
```
./minerpp
```
Note you can use the same config file as used with blago's miner-burst, but it needs to be located in the same folder as the executable and named `config.json`.

# minerpp
simple burstcoin miner for linux(even arm)
 * Ported from [miner-burst](https://github.com/Blagodarenko/miner-burst)
 * Tested on MacOS and Debian Wheezy.
 * Uses the same config file as miner-burst, but it needs to be located in the same folder as the executable and named `config.json`.

Note that CPU usage of the mining threads as well as speed (MiB/s) do not represent the correct value.

## Build
 * create a new folder named \"bin\" (`mkdir bin`)
 * switch to it (`cd bin`)
 * call `cmake ..`
 * call `make` (use `-j <number of threads>` to speedup the compiling process)
Note that you will need to compile with gcc with at lest version 5 (because it is required for `std::filesystem` to work).
Clang will not work!

Also to change the compiler run cmake with the following options `cmake -DCMAKE_C_COMPILER=<path/to/gcc> -DCMAKE_CXX_COMPILER=<path/to/g++> ..`.
Replace <path/to/gcc> with the path to your gcc installation (e.g. `/usr/local/gcc-7.2.0/bin/gcc-7.2.0`) and <path/to/g++> with the path to 
your g++ installation (e.g. `/usr/local/gcc-7.2.0/bin/g++-7.2.0`).

To get gcc/g++ 7.2.0 for your SBC try this [link](https://solarianprogrammer.com/2017/12/08/raspberry-pi-raspbian-install-gcc-compile-cpp-17-programs/)
(was tested on Odroid HC1 with DietPi and Raspberry Pi 3+ with Raspbian Wheezy). On newer versions (Stretch) just use/install
gcc-5 from your preffered package manager.


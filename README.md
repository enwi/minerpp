# minerpp
simple burstcoin miner for linux(even arm)
 * Ported from https://github.com/Blagodarenko/miner-burst.
 * Tested on MacOS and Debian Wheezy.
 * Uses the same config file as miner-burst, but it needs to be located in the same folder as the executable and named \"config.json\".

Note that CPU usage of the mining threads as well as speed (MiB/s) do not represent the correct value.

## Build
 * create a new folder named \"bin\" (mkdir bin)
 * swicth to it (cd bin)
 * call \"cmake ..\"
 * call \"make\"
Note that you will need to compile with gcc with at lest version 5 (because it is required for std::filesystem to work).
Clang will not work!

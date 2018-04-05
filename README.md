# minerpp
simple burstcoin miner for linux(even arm) 

Ported from https://github.com/Blagodarenko/miner-burst.

Uses the same config file as miner-burst, but it needs to be located in the same folder as the executable and named \"config.json\".

To build create a new folder named \"bin\" and call \"cmake ..\", then \"make\".
Note that you will need to compile with gcc and at lest version 5.
Clang will not work!

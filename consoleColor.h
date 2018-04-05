#ifndef _CONSOLECOLOR_H
#define _CONSOLECOLOR_H

#ifdef WIN32
    #include <windows.h>
#elif defined(__linux__) || defined(__APPLE__)
    #include <iostream>
#endif

namespace console
{
    namespace color
    {
        enum code
        {
        #ifdef WIN32
            FG_RED      = 12,
            FG_GREEN    = 10,
            FG_YELLOW   = 14,
            FG_BLUE     =  9,
            FG_MAGENTA  = 13,
            FG_CYAN     = 11,
            FG_WHITE    = 15,
            RESET       = 15,
        #elif defined(__linux__) || defined(__APPLE__)
            FG_RED      = 31,
            FG_GREEN    = 32,
            FG_YELLOW   = 33,
            FG_BLUE     = 34,
            FG_MAGENTA  = 35,
            FG_CYAN     = 36,
            FG_WHITE    = 37,
            RESET       =  0,
        #endif
        };
    }
    void SetColor(color::code c)
    {
    #ifdef WIN32
        HANDLE hConsole = GetStdHandle(STD_OUTPUT_HANDLE);
        SetConsoleTextAttribute(hConsole, c);
    #elif defined(__linux__) || defined(__APPLE__)
        std::cout << "\033[1;" << c << "m";
    #endif
    }
}

#endif // _CONSOLECOLOR_H

#ifndef TONLIBJSON_EXPORT_H
#define TONLIBJSON_EXPORT_H

#ifdef WIN32
    #ifdef TONLIBJSON_EXPORTS
        #define TONLIBJSON_EXPORT __declspec(dllexport)
    #else
        #define TONLIBJSON_EXPORT __declspec(dllimport)
    #endif
#else
    #define TONLIBJSON_EXPORT __attribute__((visibility("default")))
#endif

#endif
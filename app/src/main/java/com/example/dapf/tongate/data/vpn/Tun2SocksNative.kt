package com.example.dapf.tongate.data.vpn

object Tun2SocksNative {
    init {
        System.loadLibrary("tun2socks_jni")
    }

    external fun startTun2SocksNative(args: Array<String>): Int
    external fun stopTun2SocksNative()
}

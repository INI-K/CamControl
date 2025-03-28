package com.inik.phototest2

import java.nio.ByteBuffer

interface LiveViewCallback {
     fun onLiveViewFrame(jpgBuffer: ByteBuffer)
     fun onLivePhotoCaptured(filePath: String)
}
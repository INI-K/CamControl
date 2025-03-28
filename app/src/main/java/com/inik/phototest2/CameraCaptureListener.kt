package com.inik.phototest2

interface CameraCaptureListener {
    fun onPhotoCaptured(filePath: String)
    fun onCaptureFailed(errorCode: Int)
}
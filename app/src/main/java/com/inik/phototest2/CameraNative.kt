package com.inik.phototest2

object CameraNative {
    init {
        System.loadLibrary("usb")
        System.loadLibrary("gphoto2_port_iolib_usb1")   // 수정
        System.loadLibrary("gphoto2_port_iolib_disk")   // 수정
        System.loadLibrary("gphoto2_port")
        System.loadLibrary("gphoto2")
        System.loadLibrary("native-lib")
    }

    external fun testLibraryLoad(): String
    external fun getLibGphoto2Version(): String
    external fun getPortInfo(): String
    external fun initCamera(): String
    external fun listenCameraEvents(callback: CameraCaptureListener)
    external fun initCameraWithFd(fd: Int, nativeLibDir: String): Int
    external fun capturePhoto(): Int
    external fun capturePhotoAsync(callback: CameraCaptureListener)
    external fun getCameraSummary(): String
    external fun closeCamera()
    external fun detectCamera(): String
    external fun isCameraConnected(): Boolean
    external fun listCameraCapabilities(): String
    external fun requestCapture()
//    external fun startListenCameraEvents(callback: CameraCaptureListener)
    external fun stopListenCameraEvents()
    external fun cameraAutoDetect():String
    external fun buildWidgetJson():String
//    external fun capturePhotoDuringLiveView() : Int

    // --- 라이브뷰 관련 ---
    external fun startLiveView(callback: LiveViewCallback)
    external fun stopLiveView()
}
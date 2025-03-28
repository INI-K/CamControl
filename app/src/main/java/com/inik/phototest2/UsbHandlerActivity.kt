package com.inik.phototest2

import android.app.Activity
import android.app.PendingIntent
import android.content.Context
import android.content.Intent
import android.hardware.usb.UsbDevice
import android.hardware.usb.UsbManager
import android.os.Bundle
import android.util.Log
import android.widget.Toast
import com.inik.phototest2.MainActivity.Companion

class UsbHandlerActivity : Activity() {
    private lateinit var usbManager: UsbManager

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        usbManager = getSystemService(Context.USB_SERVICE) as UsbManager
        Log.d(TAG, "USB 핸들러 열림")
        // 모든 연결된 USB 기기 처리
        // 앱이 USB 연결 인텐트에 의해 실행됐는지 확인
        if (intent?.action == UsbManager.ACTION_USB_DEVICE_ATTACHED) {
            intent.getParcelableExtra<UsbDevice>(UsbManager.EXTRA_DEVICE)?.let { processUsbDevice(it) }
        } else {
            handleUsbDevices()  // 기존 연결된 기기만 처리
        }
//        finish()
    }

    private fun handleUsbDevices() {
        // 현재 연결된 모든 USB 기기 확인
        val deviceList = usbManager.deviceList
        Log.d(TAG, "디바이스 사이즈 확인 : ${deviceList.size}")
        for ((_, device) in deviceList) {
            processUsbDevice(device)
        }

        // 새로 연결되는 USB 기기 처리
        intent?.let { handleUsbIntent(it) }
    }
    private fun processUsbDevice(device: UsbDevice) {
        Log.d(TAG, "USB 기기 발견: ${device.deviceName}")
        if (!usbManager.hasPermission(device)) {
            requestUsbPermission(this, usbManager, device)
        } else {
            Log.d(TAG, "USB 권한 이미 있음")
            startMainActivity(device)
        }
    }

    private fun handleUsbIntent(intent: Intent) {
        when (intent.action) {
            UsbManager.ACTION_USB_DEVICE_ATTACHED -> {
                val device: UsbDevice? = intent.getParcelableExtra(UsbManager.EXTRA_DEVICE)
                device?.let { processUsbDevice(it) }
            }
        }
    }

    private fun requestUsbPermission(context: Context, usbManager: UsbManager, device: UsbDevice) {
        val intent = Intent(MyApp.ACTION_USB_PERMISSION).apply {
            setPackage(context.packageName)
            putExtra(UsbManager.EXTRA_DEVICE, device)        // 📌 필수
        }
        val pending = PendingIntent.getBroadcast(
            context,
            device.deviceId,
            intent,
            PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE
        )
        usbManager.requestPermission(device, pending)
        Log.d(TAG, "RQ_USB 권한 요청: ${device.deviceName}")
        finish()
    }

    private fun startMainActivity(device: UsbDevice) {
        Intent(this, MainActivity::class.java).apply {
            flags = Intent.FLAG_ACTIVITY_NEW_TASK or
                    Intent.FLAG_ACTIVITY_CLEAR_TASK or
                    Intent.FLAG_ACTIVITY_SINGLE_TOP
            putExtra(UsbManager.EXTRA_DEVICE, device)
        }.also { startActivity(it) }
        finish()
    }
    companion object {
        private const val TAG = "UsbHandlerActivity"
        private const val ACTION_USB_PERMISSION = "com.inik.phototest2.USB_PERMISSION"
    }

}
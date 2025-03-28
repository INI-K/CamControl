package com.inik.phototest2

import android.app.PendingIntent
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.hardware.usb.UsbDevice
import android.hardware.usb.UsbManager
import android.util.Log
import android.widget.Toast

class UsbPermissionReceiver : BroadcastReceiver() {
    companion object {
        private const val TAG = "UsbReceiver"
        private var lastEventTime = 0L
        private const val DEBOUNCE_MS = 500L
        private var lastDeviceName: String? = null
    }

    override fun onReceive(context: Context, intent: Intent) {
        Log.d("UsbReceiver", "수신 intent = ${intent.action}")
        Log.d("UsbReceiver", "수신 확인 = ${intent.getBooleanExtra(UsbManager.EXTRA_PERMISSION_GRANTED, false)}" )

        Log.d("UsbReceiver", "11111111111")
        val action = intent.action
        val usbManager = context.getSystemService(Context.USB_SERVICE) as UsbManager
        Log.d("UsbReceiver", "22222222222")
        val device = intent.getParcelableExtra<UsbDevice>(UsbManager.EXTRA_DEVICE) ?: return
        Log.d("UsbReceiver", "3333333333333")

//        val now = System.currentTimeMillis()
//        if (now - lastEventTime < DEBOUNCE_MS || device.deviceName == lastDeviceName) return
//        lastEventTime = now
//        lastDeviceName = if (action == UsbManager.ACTION_USB_DEVICE_ATTACHED) {
//            device.deviceName
//        } else if(action == MyApp.ACTION_USB_PERMISSION){
//            device.deviceName
//        }else{
//            null
//        }

        Log.d("UsbReceiver", "액션 확인 : $action")
        when (action) {
            UsbManager.ACTION_USB_DEVICE_ATTACHED -> {
                if (usbManager.hasPermission(device)) {
                    Log.d(TAG, "UM_USB 장치 이미 권한 있음: ${device.deviceName}")
//                    handleAttach(device, usbManager, context)
//                    showUsbPermissionDialog(context)
                    //todo main이동
                } else {
//                    Log.d("UM_UsbReceiver", "장치 권한 요청!")
//                    requestUsbPermission(context, usbManager, device)
                }
            }

            UsbManager.ACTION_USB_DEVICE_DETACHED -> {
                Log.d(TAG, "UM_USB 장치 분리됨: ${device.deviceName}")
                CameraNative.stopListenCameraEvents()
                CameraNative.closeCamera()
            }

            MyApp.ACTION_USB_PERMISSION -> {
                if (usbManager.hasPermission(device)) {
                    Log.d(TAG, "MyApp_USB 권한 허용됨: ${device.deviceName}")
//                    handleAttach(device, usbManager, context)
//                    showUsbPermissionDialog(context)
                    //todo main이동
                    startMainActvity(context)
                } else {
                    Log.d(TAG, "MyApp_USB 권한 거부됨: ${device.deviceName}")
                    Toast.makeText(context, "USB 권한이 거부되었습니다.", Toast.LENGTH_SHORT).show()
                }
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
    }

    private fun startMainActvity(context: Context){
        val activityIntent = Intent(context, MainActivity::class.java).apply {
            flags = Intent.FLAG_ACTIVITY_NEW_TASK
        }
        // Activity 실행
        context.startActivity(activityIntent)
    }
}
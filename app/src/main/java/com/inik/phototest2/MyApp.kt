package com.inik.phototest2

import android.app.Activity
import android.app.Application
import android.app.PendingIntent
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.hardware.usb.UsbManager
import android.util.Log
import java.io.File

class MyApp : Application(), Application.ActivityLifecycleCallbacks {

    companion object {
        const val ACTION_USB_PERMISSION = "com.inik.phototest2.USB_PERMISSION"
        lateinit var permissionIntent: PendingIntent
        private const val TAG = "MyApp"
    }

    // 현재 실행 중인 액티비티 수를 추적
    private var activityReferences = 0
    private var isActivityChangingConfigurations = false

    override fun onCreate() {
        super.onCreate()

        permissionIntent = PendingIntent.getBroadcast(
            this, 0, Intent(ACTION_USB_PERMISSION), PendingIntent.FLAG_IMMUTABLE
        )

        val filter = IntentFilter().apply {
            addAction(ACTION_USB_PERMISSION)
            addAction(UsbManager.ACTION_USB_DEVICE_ATTACHED)
            addAction(UsbManager.ACTION_USB_DEVICE_DETACHED)
        }

//        // ★ 플래그 추가 ★
//        registerReceiver(
//            UsbPermissionReceiver(),
//            filter,
//            Context.RECEIVER_EXPORTED
//        )
//
//        Log.d("MyApp", "MyApp 초기화 완료 — USB 리시버 등록")

        // Activity lifecycle 콜백 등록 (앱 상태 모니터링)
        registerActivityLifecycleCallbacks(this)
    }

    /**
     * 모든 사진 파일 삭제 메서드
     */
    private fun deleteAllPhotoFiles() {
        val filesDir: File = applicationContext.filesDir
        val photoFiles = filesDir.listFiles { file ->
            file.name.startsWith("photo_") && file.name.endsWith(".jpg")
        }
        photoFiles?.forEach { file ->
            if (file.delete()) {
                Log.d(TAG, "Deleted photo file: ${file.absolutePath}")
            } else {
                Log.e(TAG, "Failed to delete photo file: ${file.absolutePath}")
            }
        }
    }

    // --- ActivityLifecycleCallbacks 구현 ---
    override fun onActivityCreated(activity: Activity, savedInstanceState: android.os.Bundle?) {}

    override fun onActivityStarted(activity: Activity) {
        if (++activityReferences == 1 && !isActivityChangingConfigurations) {
            // 앱이 포그라운드로 전환됨 (필요시 여기서 작업 수행)
            Log.d(TAG, "앱이 포그라운드로 전환됨")
        }
    }

    override fun onActivityResumed(activity: Activity) {}

    override fun onActivityPaused(activity: Activity) {}

    override fun onActivityStopped(activity: Activity) {
        isActivityChangingConfigurations = activity.isChangingConfigurations
        if (--activityReferences == 0 && !isActivityChangingConfigurations) {
            // 앱이 백그라운드로 전환됨 → 사진 파일 삭제 수행
            Log.d(TAG, "앱이 백그라운드로 전환됨, 모든 사진 파일 삭제")
            deleteAllPhotoFiles()
        }
    }

    override fun onActivitySaveInstanceState(activity: Activity, outState: android.os.Bundle) {}

    override fun onActivityDestroyed(activity: Activity) {}
}
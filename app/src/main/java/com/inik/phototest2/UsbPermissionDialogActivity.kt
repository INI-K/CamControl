package com.inik.phototest2

import android.content.Intent
import android.os.Bundle
import androidx.appcompat.app.AlertDialog
import androidx.appcompat.app.AppCompatActivity

class UsbPermissionDialogActivity : AppCompatActivity() {

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        AlertDialog.Builder(this)
            .setTitle("USB 권한 승인됨")
            .setMessage("앱을 실행하시겠습니까?")
            .setPositiveButton("확인") { _, _ ->
                startActivity(
                    Intent(this, MainActivity::class.java)
                    .apply {
                        flags = Intent.FLAG_ACTIVITY_NEW_TASK or Intent.FLAG_ACTIVITY_CLEAR_TOP
                    })
                finish()
            }
            .setNegativeButton("취소") { _, _ -> finish() }
            .setCancelable(false)
            .show()
    }
}
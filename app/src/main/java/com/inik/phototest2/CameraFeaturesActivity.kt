// app/src/main/java/com/inik/phototest2/CameraFeaturesActivity.kt

package com.inik.phototest2

import android.os.Bundle
import android.util.Log
import androidx.appcompat.app.AppCompatActivity
import com.inik.phototest2.databinding.ActivityCameraFeaturesBinding
import org.json.JSONObject

class CameraFeaturesActivity : AppCompatActivity() {

    private lateinit var binding: ActivityCameraFeaturesBinding

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        binding = ActivityCameraFeaturesBinding.inflate(layoutInflater)
        setContentView(binding.root)

        loadCameraFeatures()
    }

    private fun loadCameraFeatures() {
        val jsonStr = CameraNative.listCameraAbilities()
        Log.e("기능확인", "기능 확인 : $jsonStr")
        binding.tvCameraFeatureJson.text = jsonStr

        if (jsonStr.contains("\"error\"")) return

        val obj = JSONObject(jsonStr)

        // 기본 정보
        val model        = obj.optString("model", "알 수 없음")
        val driverStatus = obj.optInt("driverStatus", -1)
        val deviceType   = obj.optInt("deviceType", -1)
        val usbVendor    = obj.optInt("usbVendor", -1)
        val usbProduct   = obj.optInt("usbProduct", -1)
        val usbClass     = obj.optInt("usbClass", -1)
        val usbSubclass  = obj.optInt("usbSubclass", -1)
        val usbProtocol  = obj.optInt("usbProtocol", -1)

        // Operation flags
        val captureImage     = obj.optBoolean("captureImage")
        val captureVideo     = obj.optBoolean("captureVideo")
        val captureAudio     = obj.optBoolean("captureAudio")
        val capturePreview   = obj.optBoolean("capturePreview")
        val config           = obj.optBoolean("config")
        val triggerCapture   = obj.optBoolean("triggerCapture")

        // File operations
        val fileDownload     = obj.optBoolean("fileDownload")
        val fileDelete       = obj.optBoolean("fileDelete")
        val filePreview      = obj.optBoolean("filePreview")
        val fileRaw          = obj.optBoolean("fileRaw")
        val fileAudio        = obj.optBoolean("fileAudio")
        val fileExif         = obj.optBoolean("fileExif")

        // Folder operations
        val deleteAll        = obj.optBoolean("deleteAll")
        val putFile          = obj.optBoolean("putFile")
        val makeDir          = obj.optBoolean("makeDir")
        val removeDir        = obj.optBoolean("removeDir")

        val bullet = "• "
        val resultText = buildString {
            append("📷 카메라 기능 목록\n\n")
            append("모델: $model\n")
            append("드라이버 상태: $driverStatus\n")
            append("디바이스 타입: $deviceType\n\n")
            append("USB Vendor/Product/Class/Subclass/Protocol:\n")
            append("$bullet $usbVendor / $usbProduct / $usbClass / $usbSubclass / $usbProtocol\n\n")

            append("▶ Remote Operations\n")
            append("$bullet 이미지 촬영: ${if (captureImage) "예" else "아니요"}\n")
            append("$bullet 비디오 촬영: ${if (captureVideo) "예" else "아니요"}\n")
            append("$bullet 오디오 녹음: ${if (captureAudio) "예" else "아니요"}\n")
            append("$bullet 라이브뷰: ${if (capturePreview) "예" else "아니요"}\n")
            append("$bullet 설정 변경: ${if (config) "예" else "아니요"}\n")
            append("$bullet 트리거 캡처: ${if (triggerCapture) "예" else "아니요"}\n\n")

            append("▶ 파일 작업\n")
            append("$bullet 다운로드: ${if (fileDownload) "예" else "아니요"}\n")
            append("$bullet 삭제: ${if (fileDelete) "예" else "아니요"}\n")
            append("$bullet 미리보기: ${if (filePreview) "예" else "아니요"}\n")
            append("$bullet RAW 지원: ${if (fileRaw) "예" else "아니요"}\n")
            append("$bullet 오디오 지원: ${if (fileAudio) "예" else "아니요"}\n")
            append("$bullet EXIF 지원: ${if (fileExif) "예" else "아니요"}\n\n")

            append("▶ 폴더 작업\n")
            append("$bullet 전체 삭제: ${if (deleteAll) "예" else "아니요"}\n")
            append("$bullet 업로드(Put): ${if (putFile) "예" else "아니요"}\n")
            append("$bullet 디렉터리 생성: ${if (makeDir) "예" else "아니요"}\n")
            append("$bullet 디렉터리 삭제: ${if (removeDir) "예" else "아니요"}")
        }

        binding.tvCameraFeatureJson.text = resultText
    }
}
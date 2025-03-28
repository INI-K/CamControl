package com.inik.phototest2

import android.app.Activity
import android.app.AlarmManager
import android.app.PendingIntent
import android.content.Context
import android.content.Intent
import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.graphics.ImageDecoder
import android.hardware.usb.UsbDevice
import android.hardware.usb.UsbManager
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.HandlerThread
import android.util.Log
import android.view.View.GONE
import android.view.View.VISIBLE
import android.widget.Button
import android.widget.ImageView
import android.widget.TextView
import android.widget.Toast
import androidx.appcompat.app.AppCompatActivity
import androidx.lifecycle.lifecycleScope
import com.davemorrissey.labs.subscaleview.ImageSource
import com.davemorrissey.labs.subscaleview.SubsamplingScaleImageView
import com.inik.phototest2.databinding.ActivityMainBinding
import com.inik.phototest2.model.GphotoWidget
import com.jakewharton.processphoenix.ProcessPhoenix
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import org.json.JSONObject
import org.w3c.dom.Text
import parseWidgetJson
import java.io.File
import java.nio.ByteBuffer
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale


class MainActivity : AppCompatActivity(), CameraCaptureListener, LiveViewCallback {

    companion object {
        const val TAG = "MainActivity"
//        const val USB_PERMISSION_ACTION = "com.inik.phototest2.USB_PERMISSION"
    }

    private lateinit var binding: ActivityMainBinding
    private lateinit var imageView: SubsamplingScaleImageView
    private lateinit var liveView: ImageView
    private lateinit var sumaryText: TextView
    private lateinit var startButton: Button
    private lateinit var statusText:TextView
    private var liveStaus = false
    private var dslrPictureListenStatus = false
    private val ioScope = CoroutineScope(Dispatchers.IO + SupervisorJob())
    private lateinit var usbManager: UsbManager
    private lateinit var device: UsbDevice
    private lateinit var decodeThread: HandlerThread
    private lateinit var decodeHandler: Handler
    private var isSummaryCleared = false



    override fun onDestroy() {
        super.onDestroy()
        Log.d(TAG, "onDestroy: 카메라 종료 호출")
        ioScope.cancel()
        CameraNative.closeCamera()
        decodeThread.quitSafely()
    }

    override fun onStart() {
        super.onStart()

    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        Log.d(TAG, "onCreate 시작")

        binding = ActivityMainBinding.inflate(layoutInflater).also {
            setContentView(it.root)
        }

        setValue()
        setInitView()
        checkUsbList()

        decodeThread = HandlerThread("DecodeThread")
        decodeThread.start()
        decodeHandler = Handler(decodeThread.looper)
    }

    private fun setValue() {
        usbManager = getSystemService(USB_SERVICE) as UsbManager
        imageView = binding.imagView
        liveView = binding.liveView
        sumaryText = binding.summaryText1
        startButton = binding.liveButton
        statusText = binding.statusText
    }

    private fun setInitView() {
        binding.permissionButton.setOnClickListener {
            cameraCheck()
        }

        binding.summaryButton.setOnClickListener {

            binding.summaryButton.setOnClickListener {
                loadCameraWidgetTree()
//                lifecycleScope.launch(Dispatchers.IO) {
////                    val summary = CameraNative.getCameraSummary()
////                    withContext(Dispatchers.Main) {
////                        sumaryText.text = summary
////                    }
//                    loadCameraWidgetTree()
//                }
            }
//            Toast.makeText(this, summary, Toast.LENGTH_LONG).show()
        }

        binding.versionButton.setOnClickListener {
            val version = CameraNative.getLibGphoto2Version()
            Log.d(TAG, "versionButton 클릭 → $version")
            Toast.makeText(this, version, Toast.LENGTH_LONG).show()
        }

        binding.asyncCaptureButton.setOnClickListener {
//            if (liveStaus) {
//                Log.d(TAG, "라이브뷰 촬영 클릭")
//                CameraNative.requestCapture()
//            }else{
//                CameraNative.capturePhoto()
//            }
            CameraNative.requestCapture()
        }

        startButton.setOnClickListener {
            startLiveView(this)
        }

        imageView.visibility = GONE
        binding.asyncCaptureButton.visibility = GONE
        binding.showFeature.setOnClickListener {
            startActivity(Intent(this@MainActivity, CameraFeaturesActivity::class.java))
        }
    }

    private fun cameraCheck() {
        device = usbManager.deviceList.values.firstOrNull()
            ?: return Toast.makeText(this, "USB 장치 없음", Toast.LENGTH_SHORT).show()
        usbManager.deviceList.values.forEach { device ->
            Log.d(
                "USB_INFO",
                "Device VID=${device.vendorId.toString(16)} PID=${device.productId.toString(16)}"
            )
        }
        intent.getParcelableExtra<UsbDevice>(UsbManager.EXTRA_DEVICE)?.let { device ->
            Log.d(TAG, "인텐트 디바이스 있음!")
            this@MainActivity.device = device
        }

        if (usbManager.hasPermission(device)) {
            Log.d(TAG, "🔔 USB 권한 있음: ${device.deviceName}")
            openDeviceAndInit(device)
            return
        } else {
            requestUsbPermission(applicationContext, usbManager, device)
            Log.d(TAG, "🔔 USB 권한 없음: ${device.deviceName}")
        }
    }


    private fun openDeviceAndInit(device: UsbDevice) {
        ioScope.launch {
            val connection = usbManager.openDevice(device)
            if (connection == null) {
                withContext(Dispatchers.Main) {
                    Toast.makeText(this@MainActivity, "USB 연결 실패", Toast.LENGTH_SHORT).show()
                    Log.e(TAG, "USB 장치 연결 실패: ${device.deviceName}")
                }
                return@launch
            }

            val fd = connection.fileDescriptor
            Log.d(TAG, "USB 파일 디스크립터 얻음: $fd")

            // 초기화는 백그라운드에서 수행
            val result = CameraNative.initCameraWithFd(fd, applicationInfo.nativeLibraryDir)

            // 무거운 작업들을 백그라운드에서 처리
            val info: String? = if (result >= 0) {
                CameraNative.detectCamera()
            } else {
                null
            }
            val summary: String? = if (result >= 0) {
                CameraNative.getCameraSummary()
            } else {
                null
            }
            val detectInfo: String = CameraNative.cameraAutoDetect()

            startListenPhoto()

            // UI 업데이트는 메인 스레드에서 실행
            withContext(Dispatchers.Main) {
                if (result >= 0) {
                    Toast.makeText(this@MainActivity, "카메라 연결 성공", Toast.LENGTH_SHORT).show()
                    Log.i(TAG, "감지된 카메라:\n$info")
                    sumaryText.text = summary
                    Log.e("인포확인 ", detectInfo)
                } else {
                    Toast.makeText(this@MainActivity, "카메라 초기화 실패 (코드: $result)", Toast.LENGTH_LONG)
                        .show()
                    Log.e(TAG, "카메라 초기화 실패: error=$result")
                    connection.close()
                    CameraNative.closeCamera()
                    restartApp(applicationContext)
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

    private fun startLiveView(callback: LiveViewCallback) {
        if (!liveStaus) {
            liveStaus = true
            liveView.visibility = VISIBLE
            lifecycleScope.launch(Dispatchers.IO) {
                stopListenPhoto()
                CameraNative.startLiveView(callback)
            }
            startButton.text = "라이브뷰 중지"
            binding.asyncCaptureButton.isClickable = true
            binding.asyncCaptureButton.visibility = VISIBLE

        } else {
            liveStaus = false

            lifecycleScope.launch(Dispatchers.IO) {
                CameraNative.stopLiveView()
                startListenPhoto()
            }
            liveView.visibility = GONE
            startButton.text = "라이브 뷰 시작"
            binding.asyncCaptureButton.isClickable = true
            binding.asyncCaptureButton.visibility = GONE
        }
    }


    private fun startListenPhoto() {
        ioScope.launch {
            if (dslrPictureListenStatus == false) {
                CameraNative.listenCameraEvents(this@MainActivity)
                Log.d(TAG, "이벤트 감지 시작")
                dslrPictureListenStatus = true
            } else {
                Log.d(TAG, "이벤트 감지중")
            }
        }
    }

    private fun stopListenPhoto() {
        if (dslrPictureListenStatus == true) {
            CameraNative.stopListenCameraEvents()
            dslrPictureListenStatus = false
            Log.d(TAG, "이벤트 감지 중지")
        } else {
            Log.d(TAG, "이벤트 감지 중지중")
        }
    }

    private fun checkUsbList() {
        val deviceList = usbManager.deviceList
        Log.d(TAG, "디바이스 사이즈 확인 : ${deviceList.size}")
        if (deviceList.size != 0) {
            cameraCheck()
        } else {
            Log.d(TAG, "연결된 카메라 없음!")
        }
    }



    override fun onLiveViewFrame(jpegBuffer: ByteBuffer) {

        jpegBuffer.rewind()
        val jpegData = ByteArray(jpegBuffer.remaining()).also { jpegBuffer.get(it) }

        decodeHandler.post {
            val bitmap = when {
                Build.VERSION.SDK_INT >= Build.VERSION_CODES.P -> {
                    // API 28+ → ImageDecoder + HARDWARE 할당
                    val source = ImageDecoder.createSource(ByteBuffer.wrap(jpegData))
                    ImageDecoder.decodeBitmap(source) { decoder, _, _ ->
                        decoder.setAllocator(ImageDecoder.ALLOCATOR_HARDWARE)
                    }
                }
                else -> {
                    // API < 26 → 기본 소프트웨어 디코딩
                    BitmapFactory.decodeByteArray(jpegData, 0, jpegData.size)
                }
            }
            runOnUiThread { liveView.setImageBitmap(bitmap) }
        }
    }

    override fun onLivePhotoCaptured(filePath: String) {
        runOnUiThread {
            Log.d(TAG, "라이브사진 찍음 → $filePath")
            Toast.makeText(this, "사진 찍음: $filePath", Toast.LENGTH_LONG).show()
//            val bmp = BitmapFactory.decodeFile(filePath)
//            imageView.setImageBitmap(bmp)
            if (imageView.visibility == GONE) {
                imageView.visibility = VISIBLE
            }
            imageView.setImage(ImageSource.uri(filePath))
        }
    }

    override fun onPhotoCaptured(filePath: String) {
        runOnUiThread {
            Log.d(TAG, "실제 사진 찍음 → $filePath")
//            Toast.makeText(this, "사진 찍음: $filePath", Toast.LENGTH_LONG).show()

            // 이미지 뷰 처리 (예: 이미 보이지 않는 경우 처리)
            if (imageView.visibility == GONE) {
                imageView.visibility = VISIBLE
            }

            // 첫 호출 시 기존 sumaryText 데이터 제거
            if (!isSummaryCleared) {
                sumaryText.text = ""
                isSummaryCleared = true
            }

            // 현재 시간 가져오기 (HH:mm:ss 형식 + 0.01초 단위)
            val now = System.currentTimeMillis()
            val date = Date(now)
            // 기본 시간 (HH:mm:ss) 포맷
            val timeStr = SimpleDateFormat("HH:mm:ss", Locale.getDefault()).format(date)
            // 밀리초의 앞 두 자리 (예: 123ms → 12)
            val hundredths = ((now % 1000) / 10).toInt()  // 0~99
            val currentTime = "$timeStr.${String.format("%02d", hundredths)}"

            val fileName = File(filePath).name
            // 보기 좋게 "[시간] 파일경로" 형태로 항목 생성
            val newEntry = "[$currentTime] $fileName"

            // 현재 sumaryText의 줄 수 확인
            val currentText = sumaryText.text.toString()
            val lines = currentText.split("\n")

            // 만약 줄 수가 100줄 이상이면 텍스트를 초기화하고 새 항목만 추가
            if (lines.size >= 100) {
                sumaryText.text = newEntry
            } else {
                // 기존 텍스트가 있으면 줄바꿈 추가
                if (currentText.isEmpty()) {
                    sumaryText.text = newEntry
                } else {
                    sumaryText.append("\n$newEntry")
                }
            }

            // 이미지 로딩 (ImageSource는 라이브러리에 따라 다를 수 있음)
            imageView.setImage(ImageSource.uri(filePath))
        }
    }

    override fun onCaptureFailed(errorCode: Int) {
        runOnUiThread {
            Log.e(TAG, "onCaptureFailed → errorCode=$errorCode")
            Toast.makeText(this, "Capture failed: $errorCode", Toast.LENGTH_LONG).show()
        }
    }
    fun restartApp(context: Context) {
        ProcessPhoenix.triggerRebirth(context);
    }
    fun loadCameraWidgetTree() {
        Log.e(TAG, "카메라 위젯 로드")
        // 1) 라이브뷰 중이면 중단
        if (liveStaus) { // 예: MainActivity에서 사용하는 라이브뷰 boolean
            CameraNative.stopLiveView()
            // stopLiveView는 내부에서 liveViewThread.join()
            // 하지만 완전히 끝나길 0.5초 정도 대기
            Thread.sleep(500)
        }

        // 2) 이벤트 리스너 중이면 중단
        if (dslrPictureListenStatus) { // 예: 이벤트 리스너 활성화 여부
            CameraNative.stopListenCameraEvents()
            // stopListenCameraEvents에서 이벤트 스레드 join
            Thread.sleep(500)
        }

        // 3) buildWidgetJsonWithRetry() 호출
        val json = CameraNative.buildWidgetJson()
        if (json.contains("\"error\"")) {
            Log.e("MainActivity", "Error in JSON: $json")
            return
        }
        Log.d("MainActivity", "Widget JSON:\n$json")

        // 3) org.json 으로 파싱json
        val rootObj = JSONObject(json)
        val rootWidget = parseWidgetJson(rootObj)  // 재귀 함수 호출

        // 이제 rootWidget 안에
        // name, label, type, choices, children(...) 등 트리 구조가 전부 들어있음!

        // 4) UI에 표시 or RecyclerView로 펼치기
        showWidgetInUI(rootWidget)
    }

    // 예: 화면에 간단히 로그로 찍어보기 (재귀)
    fun showWidgetInUI(widget: GphotoWidget, depth: Int = 0) {
        val indent = "  ".repeat(depth)
        Log.d("UI", "$indent name=${widget.name}, label=${widget.label}, type=${widget.type}")
        if (widget.choices.isNotEmpty()) {
            Log.d("UI", "$indent  choices=${widget.choices.joinToString()}")
        }
        // 자식 위젯
        widget.children.forEach { child ->
            showWidgetInUI(child, depth+1)
        }
    }
}
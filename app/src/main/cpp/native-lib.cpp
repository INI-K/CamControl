// app/src/main/cpp/CameraNative.cpp

#include <jni.h>
#include <android/log.h>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <ctime>

// --- gPhoto2 헤더 ---
#include <gphoto2/gphoto2.h>
#include <gphoto2/gphoto2-camera.h>
#include <gphoto2/gphoto2-context.h>
#include <gphoto2/gphoto2-port.h>
#include <gphoto2/gphoto2-port-version.h>
#include <gphoto2/gphoto2-port-result.h>
#include <gphoto2/gphoto2-version.h>
#include <gphoto2/gphoto2-widget.h>
#include <gphoto2/gphoto2-list.h>

#ifndef TAG
#define TAG "CameraNative"
#endif

#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

// ----------------------------------------------------------------------------
// 전역/공유 자원
// ----------------------------------------------------------------------------
static std::mutex cameraMutex;
static GPContext *context = nullptr;
static Camera *camera = nullptr;
static JavaVM *gJvm = nullptr;

// 이벤트 리스너 관련
static std::atomic_bool eventListenerRunning(false);
static std::thread eventListenerThread;
static std::mutex eventCvMtx;
static std::condition_variable eventCv;

// 라이브뷰 관련
static std::atomic_bool liveViewRunning(false);
static std::thread liveViewThread;
static jobject gCallback = nullptr;
static std::atomic_bool captureRequested(false);

// gPhoto2에 공식 정의되지 않은 확장 상수 (사용자 임의 정의)
#ifndef GP_ERROR_IO_IN_PROGRESS
#define GP_ERROR_IO_IN_PROGRESS (-110)
#endif
#define GP_FILE_OPERATION_READ  (1 << 0) // 사용자 확장
#define GP_FILE_OPERATION_WRITE (1 << 2) // 1<<1은 DELETE이므로 1<<2를 WRITE로 사용

// ----------------------------------------------------------------------------
// JSON 생성 보조 함수
// ----------------------------------------------------------------------------
static void jsonAppend(std::ostringstream &oss, const char *key, bool value, bool &first) {
    if (!first) oss << ",";
    oss << "\"" << key << "\":" << (value ? "true" : "false");
    first = false;
}

static void jsonAppend(std::ostringstream &oss, const char *key, const char *value, bool &first) {
    if (!first) oss << ",";
    oss << "\"" << key << "\":\"" << (value ? value : "") << "\"";
    first = false;
}

// ----------------------------------------------------------------------------
// gPhoto2 메시지/에러 콜백
// ----------------------------------------------------------------------------
static void message_callback(GPContext *context, const char *str, void *data) {
    LOGE("libgphoto2 message: %s", str);
}

static void error_callback(GPContext *context, const char *str, void *data) {
    LOGE("libgphoto2 error: %s", str);
}

// ----------------------------------------------------------------------------
// 간단 라이브뷰 지원 체크 (liveviewsize 위젯 존재 여부로 가정)
// ----------------------------------------------------------------------------
static bool checkLiveViewSupport(Camera *cam, GPContext *ctx) {
    CameraWidget *config = nullptr;
    int ret = gp_camera_get_config(cam, &config, ctx);
    if (ret < GP_OK || !config) return false;

    CameraWidget *lvWidget = nullptr;
    ret = gp_widget_get_child_by_name(config, "liveviewsize", &lvWidget);
    gp_widget_free(config);

    return (ret >= GP_OK && lvWidget != nullptr);
}

// ----------------------------------------------------------------------------
// 특수 문자 이스케이프 (JSON)
static std::string escapeJsonString(const std::string &s) {
    std::string out;
    out.reserve(s.size() + 20);
    for (char c: s) {
        switch (c) {
            case '\\':
                out += "\\\\";
                break;
            case '\"':
                out += "\\\"";
                break;
            default:
                out.push_back(c);
                break;
        }
    }
    return out;
}

// ----------------------------------------------------------------------------
// CameraWidget 정보를 JSON으로 재귀 변환
// ----------------------------------------------------------------------------
static const char *widgetTypeToString(CameraWidgetType type) {
    switch (type) {
        case GP_WIDGET_WINDOW:
            return "WINDOW";
        case GP_WIDGET_SECTION:
            return "SECTION";
        case GP_WIDGET_TEXT:
            return "TEXT";
        case GP_WIDGET_RANGE:
            return "RANGE";
        case GP_WIDGET_TOGGLE:
            return "TOGGLE";
        case GP_WIDGET_RADIO:
            return "RADIO";
        case GP_WIDGET_MENU:
            return "MENU";
        case GP_WIDGET_BUTTON:
            return "BUTTON";
        default:
            return "UNKNOWN";
    }
}

static std::string buildWidgetJson(CameraWidget *widget) {
    // 1) name, label, type 구하기
    const char *nameC = nullptr, *labelC = nullptr;
    gp_widget_get_name(widget, &nameC);
    gp_widget_get_label(widget, &labelC);
    std::string name = (nameC ? nameC : "");
    std::string label = (labelC ? labelC : "");

    CameraWidgetType wtype;
    gp_widget_get_type(widget, &wtype);

    // 2) JSON 시작: { "name":"...", "label":"...", "type":"...", ...
    std::ostringstream oss;
    oss << "{"
        << "\"name\":\"" << escapeJsonString(name) << "\","
        << "\"label\":\"" << escapeJsonString(label) << "\",";

    // type 문자열화
    const char *typeStr = widgetTypeToString(wtype);
    oss << "\"type\":\"" << typeStr << "\",";

    // 3) choices 배열 (RADIO, MENU 등일 때만)
    if (wtype == GP_WIDGET_RADIO || wtype == GP_WIDGET_MENU) {
        int count = gp_widget_count_choices(widget);
        oss << "\"choices\":[";
        for (int i = 0; i < count; i++) {
            const char *choiceC = nullptr;
            gp_widget_get_choice(widget, i, &choiceC);
            std::string choice = (choiceC ? choiceC : "");
            oss << (i == 0 ? "" : ",") << "\"" << escapeJsonString(choice) << "\"";
        }
        oss << "],";
    }

    // 4) children 배열 (하위 위젯 재귀)
    int childCount = gp_widget_count_children(widget);
    oss << "\"children\":[";
    for (int i = 0; i < childCount; i++) {
        CameraWidget *child = nullptr;
        if (gp_widget_get_child(widget, i, &child) == GP_OK && child) {
            if (i > 0) oss << ",";
            oss << buildWidgetJson(child);
        }
    }
    oss << "]";

    // 5) 객체 끝
    oss << "}";
    return oss.str();
}

// ----------------------------------------------------------------------------
// JNI_OnLoad
// ----------------------------------------------------------------------------
extern "C" JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *vm, void *) {
    gJvm = vm;
    context = gp_context_new();

    gp_context_set_message_func(context, message_callback, nullptr);
    gp_context_set_error_func(context, error_callback, nullptr);

    LOGD("JNI_OnLoad -> gJvm=%p, gp_context_new 완료", gJvm);
    return JNI_VERSION_1_6;
}

// ----------------------------------------------------------------------------
// 기본 카메라 초기화/종료
// ----------------------------------------------------------------------------
extern "C" JNIEXPORT jstring JNICALL
Java_com_inik_phototest2_CameraNative_initCamera(JNIEnv *env, jobject) {
    LOGD("initCamera 호출");
    std::lock_guard<std::mutex> lock(cameraMutex);

    int ret = gp_camera_new(&camera);
    if (ret < GP_OK) {
        LOGE("initCamera: gp_camera_new 실패 -> %s", gp_result_as_string(ret));
        return env->NewStringUTF(gp_result_as_string(ret));
    }
    ret = gp_camera_init(camera, context);
    LOGD("initCamera - gp_camera_init ret=%d (%s)", ret, gp_result_as_string(ret));

    return env->NewStringUTF(gp_result_as_string(ret));
}

extern "C" JNIEXPORT void JNICALL
Java_com_inik_phototest2_CameraNative_closeCamera(JNIEnv *, jobject) {
    LOGD("closeCamera 호출");
    std::lock_guard<std::mutex> lock(cameraMutex);

    if (camera) {
        gp_camera_exit(camera, context);
        gp_camera_free(camera);
        camera = nullptr;
        LOGD("closeCamera: camera freed");
    }
    if (context) {
        gp_context_unref(context);
        context = nullptr;
        LOGD("closeCamera: context unref");
    }
    LOGD("closeCamera 완료");
}

// ----------------------------------------------------------------------------
// FD를 통한 카메라 초기화(안드로이드 USB) - openDeviceAndInit()
// ----------------------------------------------------------------------------
extern "C" JNIEXPORT jint JNICALL
Java_com_inik_phototest2_CameraNative_initCameraWithFd(
        JNIEnv *env, jobject, jint fd, jstring libDir_) {

    const char *libDir = env->GetStringUTFChars(libDir_, nullptr);
    LOGD("initCameraWithFd 시작: fd=%d, libDir=%s", fd, libDir);

    // 환경변수 설정 (libgphoto2 camlibs/iolibs)
    setenv("CAMLIBS", libDir, 1);
    setenv("IOLIBS", libDir, 1);
    setenv("CAMLIBS_PREFIX", "libgphoto2_camlib_", 1);
    setenv("IOLIBS_PREFIX", "libgphoto2_port_iolib_", 1);

    std::lock_guard<std::mutex> lock(cameraMutex);
    if (camera) {
        gp_camera_exit(camera, context);
        gp_camera_free(camera);
        camera = nullptr;
    }

    // fd 설정
    int ret = gp_port_usb_set_sys_device(fd);
    LOGD("initCameraWithFd gp_port_usb_set_sys_device ret=%d (%s)", ret, gp_result_as_string(ret));
    if (ret < GP_OK) {
        env->ReleaseStringUTFChars(libDir_, libDir);
        return ret;
    }

    int finalRet = -1;
    // 재시도 (3회)
    for (int i = 0; i < 3; ++i) {
        ret = gp_camera_new(&camera);
        if (ret < GP_OK) {
            finalRet = ret;
            continue;
        }

        ret = gp_camera_init(camera, context);
        if (ret == GP_OK) {
            finalRet = ret;
            break;
        } else {
            gp_camera_free(camera);
            camera = nullptr;
            finalRet = ret;
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
    }

    env->ReleaseStringUTFChars(libDir_, libDir);
    LOGD("initCameraWithFd done -> ret=%d", finalRet);
    return finalRet;
}

// ----------------------------------------------------------------------------
// 카메라 감지, 요약 등
// ----------------------------------------------------------------------------
extern "C" JNIEXPORT jstring JNICALL
Java_com_inik_phototest2_CameraNative_detectCamera(JNIEnv *env, jobject) {
    LOGD("detectCamera 호출");

    CameraList *cl = nullptr;
    gp_list_new(&cl);

    int ret = gp_camera_autodetect(cl, context);
    int count = gp_list_count(cl);

    std::ostringstream oss;
    if (ret >= GP_OK && count > 0) {
        for (int i = 0; i < count; i++) {
            const char *name, *port;
            gp_list_get_name(cl, i, &name);
            gp_list_get_value(cl, i, &port);
            oss << (name ? name : "Unknown") << " @ " << (port ? port : "Unknown") << "\n";
        }
    } else {
        oss << "No camera detected";
    }
    gp_list_free(cl);

    return env->NewStringUTF(oss.str().c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inik_phototest2_CameraNative_getCameraSummary(JNIEnv *env, jobject) {
    LOGD("getCameraSummary");
    std::lock_guard<std::mutex> lock(cameraMutex);

    if (!camera) {
        return env->NewStringUTF("Camera not initialized");
    }

    CameraText txt;
    int ret = gp_camera_get_summary(camera, &txt, context);
    if (ret < GP_OK) {
        return env->NewStringUTF(gp_result_as_string(ret));
    }

    return env->NewStringUTF(txt.text);
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_inik_phototest2_CameraNative_isCameraConnected(JNIEnv *env, jobject) {
    LOGD("isCameraConnected 호출");

    CameraList *cl = nullptr;
    gp_list_new(&cl);

    int ret = gp_camera_autodetect(cl, context);
    int count = gp_list_count(cl);
    gp_list_free(cl);

    bool connected = (ret >= GP_OK && count > 0);
    return connected;
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inik_phototest2_CameraNative_cameraAutoDetect(JNIEnv *env, jobject) {
    GPContext *ctx = gp_context_new();
    gp_context_set_message_func(ctx, message_callback, nullptr);
    gp_context_set_error_func(ctx, error_callback, nullptr);

    CameraList *list;
    int ret = gp_list_new(&list);
    if (ret < GP_OK) {
        gp_context_unref(ctx);
        return env->NewStringUTF("Failed to create camera list");
    }

    ret = gp_camera_autodetect(list, ctx);
    if (ret < GP_OK) {
        gp_list_free(list);
        gp_context_unref(ctx);
        return env->NewStringUTF("Camera autodetect failed");
    }

    int count = gp_list_count(list);
    char result[1024] = {0};
    snprintf(result, sizeof(result), "Detected %d cameras\n", count);

    for (int i = 0; i < count; i++) {
        const char *name, *port;
        gp_list_get_name(list, i, &name);
        gp_list_get_value(list, i, &port);

        char buffer[256];
        snprintf(buffer, sizeof(buffer), "Camera: %s, Port: %s\n",
                 (name ? name : "Unknown"), (port ? port : "Unknown"));
        strncat(result, buffer, sizeof(result) - strlen(result) - 1);
    }
    LOGE("%s", result);

    gp_list_free(list);
    gp_context_unref(ctx);
    return env->NewStringUTF(result);
}

// ----------------------------------------------------------------------------
// gPhoto2 라이브러리/포트 테스트용
// ----------------------------------------------------------------------------
extern "C" JNIEXPORT jstring JNICALL
Java_com_inik_phototest2_CameraNative_testLibraryLoad(JNIEnv *env, jobject) {
    GPPortInfoList *pil = nullptr;
    int ret = gp_port_info_list_new(&pil);
    if (ret < GP_OK) {
        return env->NewStringUTF(gp_result_as_string(ret));
    }

    ret = gp_port_info_list_load(pil);
    gp_port_info_list_free(pil);

    return env->NewStringUTF(ret >= GP_OK ? "OK" : gp_result_as_string(ret));
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inik_phototest2_CameraNative_getLibGphoto2Version(JNIEnv *env, jobject) {
    const char **v = gp_library_version(GP_VERSION_SHORT);
    return env->NewStringUTF((v && v[0]) ? v[0] : "Unknown");
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_inik_phototest2_CameraNative_getPortInfo(JNIEnv *env, jobject) {
    GPPortInfoList *pil = nullptr;
    gp_port_info_list_new(&pil);
    int ret = gp_port_info_list_load(pil);

    std::ostringstream oss;
    int count = gp_port_info_list_count(pil);
    for (int i = 0; i < count; i++) {
        GPPortInfo info;
        if (gp_port_info_list_get_info(pil, i, &info) == GP_OK) {
            const char *name, *path;
            gp_port_info_get_name(info, (char **) &name);
            gp_port_info_get_path(info, (char **) &path);
            oss << (name ? name : "?") << " @ " << (path ? path : "?") << "\n";
        }
    }
    gp_port_info_list_free(pil);

    return env->NewStringUTF(oss.str().c_str());
}

// ----------------------------------------------------------------------------
// 사진 촬영(동기)
// ----------------------------------------------------------------------------
extern "C" JNIEXPORT jint JNICALL
Java_com_inik_phototest2_CameraNative_capturePhoto(JNIEnv *env, jobject, jstring) {
    LOGD("capturePhoto");
    std::lock_guard<std::mutex> lock(cameraMutex);

    if (!camera) {
        return GP_ERROR;
    }

    CameraFilePath cfp;
    int ret = gp_camera_capture(camera, GP_CAPTURE_IMAGE, &cfp, context);
    if (ret < GP_OK) {
        return ret;
    }

    CameraFile *file;
    gp_file_new(&file);
    int getRet = gp_camera_file_get(camera, cfp.folder, cfp.name, GP_FILE_TYPE_NORMAL, file,
                                    context);
    if (getRet < GP_OK) {
        gp_file_free(file);
        return getRet;
    }

    // 저장 경로 예시
    char savePath[128];
    snprintf(savePath, sizeof(savePath),
             "/data/data/com.inik.phototest2/files/photo_%lld.jpg",
             (long long) std::time(nullptr));

    gp_file_save(file, savePath);
    gp_file_free(file);

    LOGD("capturePhoto -> 저장 완료: %s", savePath);
    return ret;
}

// 비동기 촬영
extern "C" JNIEXPORT void JNICALL
Java_com_inik_phototest2_CameraNative_capturePhotoAsync(JNIEnv *env, jobject, jobject cb) {
    LOGD("capturePhotoAsync 호출");
    jobject globalCb = env->NewGlobalRef(cb);

    JavaVM *vm;
    env->GetJavaVM(&vm);

    std::thread([vm, globalCb]() {
        JNIEnv *threadEnv;
        vm->AttachCurrentThread(&threadEnv, nullptr);

        jstring dummyPath = threadEnv->NewStringUTF("unused");
        jint result = Java_com_inik_phototest2_CameraNative_capturePhoto(threadEnv, nullptr,
                                                                         dummyPath);

        jclass cls = threadEnv->GetObjectClass(globalCb);
        if (result >= GP_OK) {
            jmethodID m = threadEnv->GetMethodID(cls, "onPhotoCaptured", "(Ljava/lang/String;)V");
            jstring path = threadEnv->NewStringUTF(
                    "/data/data/com.inik.phototest2/files/photo.jpg");
            threadEnv->CallVoidMethod(globalCb, m, path);
            threadEnv->DeleteLocalRef(path);
        } else {
            jmethodID m = threadEnv->GetMethodID(cls, "onCaptureFailed", "(I)V");
            threadEnv->CallVoidMethod(globalCb, m, result);
        }

        threadEnv->DeleteLocalRef(dummyPath);
        threadEnv->DeleteGlobalRef(globalCb);
        vm->DetachCurrentThread();
    }).detach();
}

// ----------------------------------------------------------------------------
// Camera 이벤트(파일 추가 등) 리스너
// ----------------------------------------------------------------------------
static void callJavaPhotoCallback(JNIEnv *env, jobject callbackObj, const char *path) {
    jclass cls = env->GetObjectClass(callbackObj);
    if (!cls) return;

    jmethodID mid = env->GetMethodID(cls, "onPhotoCaptured", "(Ljava/lang/String;)V");
    if (!mid) return;

    jstring jPath = env->NewStringUTF(path);
    env->CallVoidMethod(callbackObj, mid, jPath);
    env->DeleteLocalRef(jPath);
}

extern "C" JNIEXPORT void JNICALL
Java_com_inik_phototest2_CameraNative_listenCameraEvents(JNIEnv *env, jobject, jobject callback) {
    if (eventListenerRunning.load()) {
        LOGD("listenCameraEvents: 이미 실행 중");
        return;
    }

    jobject globalCb = env->NewGlobalRef(callback);
    JavaVM *vm;
    env->GetJavaVM(&vm);

    eventListenerRunning.store(true);

    eventListenerThread = std::thread([vm, globalCb]() {
        JNIEnv *threadEnv;
        if (vm->AttachCurrentThread(&threadEnv, nullptr) != JNI_OK) {
            LOGE("listenCameraEvents: AttachCurrentThread 실패");
            return;
        }

        static std::atomic<int> photoCounter{0};

        while (eventListenerRunning.load()) {
            {
                std::lock_guard<std::mutex> lock(cameraMutex);
                if (!camera) {
                    LOGE("listenCameraEvents: camera=null -> 종료");
                    break;
                }
            }

            CameraEventType type;
            void *data = nullptr;
            int ret = gp_camera_wait_for_event(camera, 5000, &type, &data, context);
            if (!eventListenerRunning.load()) break;
            if (ret != GP_OK) {
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                continue;
            }

            if (type == GP_EVENT_FILE_ADDED) {
                CameraFilePath *cfp = static_cast<CameraFilePath *>(data);
                LOGD("새 파일 추가: %s/%s", cfp->folder, cfp->name);

                char pathBuf[128];
                auto now = std::chrono::system_clock::now();
                auto nowMs = std::chrono::time_point_cast<std::chrono::milliseconds>(now);
                long long millis = nowMs.time_since_epoch().count();
                int count = photoCounter.fetch_add(1);

                snprintf(pathBuf, sizeof(pathBuf),
                         "/data/data/com.inik.phototest2/files/photo_%lld_%d.jpg",
                         millis, count);

                CameraFile *file;
                gp_file_new(&file);

                int getRet = -1;
                for (int i = 0; i < 5; ++i) {
                    getRet = gp_camera_file_get(camera, cfp->folder, cfp->name,
                                                GP_FILE_TYPE_NORMAL, file, context);
                    if (getRet >= GP_OK) break;
                    std::this_thread::sleep_for(std::chrono::milliseconds(300));
                }

                if (getRet >= GP_OK) {
                    gp_file_save(file, pathBuf);
                    LOGD("listenCameraEvents: 저장 완료 -> %s", pathBuf);

                    jclass cls = threadEnv->GetObjectClass(globalCb);
                    jmethodID m = threadEnv->GetMethodID(cls, "onPhotoCaptured",
                                                         "(Ljava/lang/String;)V");
                    jstring pathStr = threadEnv->NewStringUTF(pathBuf);
                    threadEnv->CallVoidMethod(globalCb, m, pathStr);
                    threadEnv->DeleteLocalRef(pathStr);
                } else {
                    LOGE("listenCameraEvents: 사진 가져오기 실패 -> %s", gp_result_as_string(getRet));

                    jclass cls = threadEnv->GetObjectClass(globalCb);
                    jmethodID m = threadEnv->GetMethodID(cls, "onCaptureFailed", "(I)V");
                    threadEnv->CallVoidMethod(globalCb, m, getRet);
                }
                gp_file_free(file);

            } else if (type == GP_EVENT_CAPTURE_COMPLETE) {
                // 촬영 완료 이벤트
                LOGD("listenCameraEvents: CAPTURE_COMPLETE");
            }

            std::unique_lock<std::mutex> lk(eventCvMtx);
            eventCv.wait_for(lk, std::chrono::milliseconds(100),
                             [] { return !eventListenerRunning.load(); });
        }

        threadEnv->DeleteGlobalRef(globalCb);
        vm->DetachCurrentThread();
    });
}

extern "C" JNIEXPORT void JNICALL
Java_com_inik_phototest2_CameraNative_stopListenCameraEvents(JNIEnv *env, jobject) {
    LOGD("stopListenCameraEvents: 호출");
    eventListenerRunning.store(false);
    eventCv.notify_all();

    std::thread([]() {
        if (eventListenerThread.joinable()) {
            eventListenerThread.join();
            LOGD("stopListenCameraEvents: 정상 종료");
        }
    }).detach();
    LOGD("stopListenCameraEvents: 요청 완료");
}

// ----------------------------------------------------------------------------
// 라이브뷰
// ----------------------------------------------------------------------------
static void liveViewLoop() {
    JNIEnv *env;
    gJvm->AttachCurrentThread(&env, nullptr);

    CameraFile *file = nullptr;
    gp_file_new(&file);

    while (liveViewRunning.load()) {
        {
            std::lock_guard<std::mutex> lock(cameraMutex);
            if (!camera) {
                LOGE("liveViewLoop: camera=null -> 종료");
                break;
            }

            int pret = gp_camera_capture_preview(camera, file, context);
            if (pret < GP_OK) {
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                continue;
            }

            const char *data = nullptr;
            unsigned long size = 0;
            gp_file_get_data_and_size(file, &data, &size);

            if (!gCallback) {
                LOGE("liveViewLoop: gCallback is null");
                break;
            }

            // onLiveViewFrame(ByteBuffer)
            jclass cls = env->GetObjectClass(gCallback);
            if (!cls) {
                LOGE("liveViewLoop: callback class not found");
            } else {
                jmethodID mid = env->GetMethodID(cls, "onLiveViewFrame",
                                                 "(Ljava/nio/ByteBuffer;)V");
                if (mid) {
                    jobject byteBuffer = env->NewDirectByteBuffer((void *) data, size);
                    env->CallVoidMethod(gCallback, mid, byteBuffer);
                    env->DeleteLocalRef(byteBuffer);
                }
            }

            // 촬영 요청이 온 경우
            if (captureRequested.exchange(false)) {
                CameraFilePath cfp;
                int cret = gp_camera_capture(camera, GP_CAPTURE_IMAGE, &cfp, context);
                if (cret >= GP_OK) {
                    CameraFile *photoFile;
                    gp_file_new(&photoFile);

                    gp_camera_file_get(camera, cfp.folder, cfp.name, GP_FILE_TYPE_NORMAL,
                                       photoFile, context);

                    char path[128];
                    snprintf(path, sizeof(path),
                             "/data/data/com.inik.phototest2/files/photo_%lld.jpg",
                             (long long) time(nullptr));
                    gp_file_save(photoFile, path);
                    gp_file_free(photoFile);

                    // onLivePhotoCaptured(...) 호출
                    jmethodID mid2 = env->GetMethodID(cls, "onLivePhotoCaptured",
                                                      "(Ljava/lang/String;)V");
                    if (mid2) {
                        jstring jPath = env->NewStringUTF(path);
                        env->CallVoidMethod(gCallback, mid2, jPath);
                        env->DeleteLocalRef(jPath);
                    }
                }
            }
        }
        gp_file_free(file);
        gp_file_new(&file);
        std::this_thread::sleep_for(std::chrono::milliseconds(42));
    }

    gp_file_free(file);
    gJvm->DetachCurrentThread();
}

extern "C" JNIEXPORT void JNICALL
Java_com_inik_phototest2_CameraNative_startLiveView(JNIEnv *env, jobject, jobject callback) {
    LOGD("startLiveView 호출");

    if (liveViewRunning.load()) {
        LOGD("startLiveView: 이미 라이브뷰 실행중");
        return;
    }

    {
        std::lock_guard<std::mutex> lock(cameraMutex);
        if (!camera) {
            LOGE("startLiveView: camera not initialized!");
            return;
        }
    }

    gCallback = env->NewGlobalRef(callback);
    liveViewRunning.store(true);
    liveViewThread = std::thread(liveViewLoop);
    LOGD("startLiveView -> 라이브뷰 스레드 시작 완료");
}

extern "C" JNIEXPORT void JNICALL
Java_com_inik_phototest2_CameraNative_stopLiveView(JNIEnv *env, jobject) {
    LOGD("stopLiveView 호출");
    liveViewRunning.store(false);

    if (liveViewThread.joinable()) {
        liveViewThread.join();
    }

    if (gCallback) {
        env->DeleteGlobalRef(gCallback);
        gCallback = nullptr;
    }
    LOGD("stopLiveView 완료");
}

extern "C" JNIEXPORT void JNICALL
Java_com_inik_phototest2_CameraNative_requestCapture(JNIEnv *env, jobject) {
    LOGD("requestCapture -> captureRequested=true");
    captureRequested.store(true);
}

// ----------------------------------------------------------------------------
// 카메라 기능(JSON) 반환
// ----------------------------------------------------------------------------
extern "C"
JNIEXPORT jstring JNICALL
Java_com_inik_phototest2_CameraNative_listCameraAbilities(JNIEnv *env, jclass) {
    std::lock_guard<std::mutex> lock(cameraMutex);
    if (!camera) return env->NewStringUTF("{\"error\":\"Camera not initialized\"}");

    CameraAbilitiesList *alist = nullptr;
    gp_abilities_list_new(&alist);
    gp_abilities_list_load(alist, context);

    CameraAbilities realAbilities;
    gp_camera_get_abilities(camera, &realAbilities);
    int idx = gp_abilities_list_lookup_model(alist, realAbilities.model);

    std::ostringstream oss;
    if (idx < 0) {
        oss << "{\"error\":\"Model not found: " << realAbilities.model << "\"}";
    } else {
        CameraAbilities cap;
        gp_abilities_list_get_abilities(alist, idx, &cap);

        oss << "{";
        bool first = true;

        // 기본 정보
        jsonAppend(oss, "model", cap.model, first);
        jsonAppend(oss, "driverStatus", static_cast<int>(cap.status), first);
        jsonAppend(oss, "deviceType", static_cast<int>(cap.device_type), first);
        jsonAppend(oss, "usbVendor", cap.usb_vendor, first);
        jsonAppend(oss, "usbProduct", cap.usb_product, first);
        jsonAppend(oss, "usbClass", cap.usb_class, first);
        jsonAppend(oss, "usbSubclass", cap.usb_subclass, first);
        jsonAppend(oss, "usbProtocol", cap.usb_protocol, first);

        // Operation bitmasks
        jsonAppend(oss, "captureImage",      (cap.operations & GP_OPERATION_CAPTURE_IMAGE), first);
        jsonAppend(oss, "captureVideo",      (cap.operations & GP_OPERATION_CAPTURE_VIDEO), first);
        jsonAppend(oss, "captureAudio",      (cap.operations & GP_OPERATION_CAPTURE_AUDIO), first);
        jsonAppend(oss, "capturePreview",    (cap.operations & GP_OPERATION_CAPTURE_PREVIEW), first);
        jsonAppend(oss, "config",            (cap.operations & GP_OPERATION_CONFIG), first);
        jsonAppend(oss, "triggerCapture",    (cap.operations & GP_OPERATION_TRIGGER_CAPTURE), first);

        // File operations
        jsonAppend(oss, "fileDownload",      true, first); // always supported if listed
        jsonAppend(oss, "fileDelete",        (cap.file_operations & GP_FILE_OPERATION_DELETE), first);
        jsonAppend(oss, "filePreview",       (cap.file_operations & GP_FILE_OPERATION_PREVIEW), first);
        jsonAppend(oss, "fileRaw",           (cap.file_operations & GP_FILE_OPERATION_RAW), first);
        jsonAppend(oss, "fileAudio",         (cap.file_operations & GP_FILE_OPERATION_AUDIO), first);
        jsonAppend(oss, "fileExif",          (cap.file_operations & GP_FILE_OPERATION_EXIF), first);

        // Folder operations
        jsonAppend(oss, "deleteAll",         (cap.folder_operations & GP_FOLDER_OPERATION_DELETE_ALL), first);
        jsonAppend(oss, "putFile",           (cap.folder_operations & GP_FOLDER_OPERATION_PUT_FILE), first);
        jsonAppend(oss, "makeDir",           (cap.folder_operations & GP_FOLDER_OPERATION_MAKE_DIR), first);
        jsonAppend(oss, "removeDir",         (cap.folder_operations & GP_FOLDER_OPERATION_REMOVE_DIR), first);

        oss << "}";
    }

    gp_abilities_list_free(alist);
    return env->NewStringUTF(oss.str().c_str());
}

// ----------------------------------------------------------------------------
// 카메라 위젯 트리 JSON 빌드
// ----------------------------------------------------------------------------
extern "C" JNIEXPORT jstring JNICALL
Java_com_inik_phototest2_CameraNative_buildWidgetJson(JNIEnv *env, jobject) {
    std::lock_guard<std::mutex> lock(cameraMutex);
    if (!camera) {
        return env->NewStringUTF("{\"error\":\"Camera not initialized\"}");
    }

    // 최대 5회 재시도
    const int maxRetries = 5;
    const int delayMs = 500;

    CameraWidget *config = nullptr;
    int ret = -1;
    for (int i = 0; i < maxRetries; i++) {
        ret = gp_camera_get_config(camera, &config, context);
        if (ret == GP_OK) {
            break;
        } else if (ret == GP_ERROR_IO_IN_PROGRESS) {
            if (config) {
                gp_widget_free(config);
                config = nullptr;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
        } else {
            break;
        }
    }

    if (ret < GP_OK || !config) {
        std::ostringstream oss;
        oss << "{\"error\":\"gp_camera_get_config failed: "
            << gp_result_as_string(ret) << "\"}";
        return env->NewStringUTF(oss.str().c_str());
    }

    std::string json = buildWidgetJson(config);
    gp_widget_free(config);
    return env->NewStringUTF(json.c_str());
}
//#include <jni.h>
//#include <android/log.h>
//#include <mutex>
//#include <sstream>
//#include <string>
//#include <thread>
//#include <atomic>
//#include <chrono>
//#include <condition_variable>
//#include <ctime>
//
//#include <gphoto2/gphoto2.h>
//#include <gphoto2/gphoto2-camera.h>
//#include <gphoto2/gphoto2-context.h>
//#include <gphoto2/gphoto2-port.h>
//#include <gphoto2/gphoto2-port-version.h>
//#include <gphoto2/gphoto2-port-result.h>
//#include <gphoto2/gphoto2-version.h>
//#include <gphoto2/gphoto2-widget.h>
//#include <gphoto2/gphoto2-list.h>
//#include <gphoto2/gphoto2-widget.h>
////#include "so_list.h"
//
//#define TAG "CameraNative"
//#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, TAG, __VA_ARGS__)
//#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)
//
//// 전역 동기화 객체
//std::condition_variable cv;
//std::mutex cv_mtx;
//static std::mutex mtx;
//
//// 카메라 제어 전역 변수
//static GPContext *context = nullptr;
//static Camera *camera = nullptr;
//
//// JNI 전역 변수
//static JavaVM *gJvm = nullptr;
//
//// ===== 라이브뷰 관련 전역 변수 =====
//static std::atomic_bool liveViewRunning(false);
//static std::thread liveViewThread;
//static jobject gLiveViewCallback = nullptr;
//static std::atomic_bool captureRequested(false);
//static jobject gCallback = nullptr;
//
//// ===== 이벤트 리스너 관련 전역 변수 =====
//static std::atomic_bool eventListenerRunning(false);
//static std::thread eventListenerThread;
//static std::mutex eventCvMtx;
//static std::mutex cameraMutex;
//static std::condition_variable eventCv;
//
//// 조건부 정의: 사용 중인 libgphoto2에 해당 상수가 없으면 정의 (예시 값)
//#ifndef GP_ERROR_IO_IN_PROGRESS
//#define GP_ERROR_IO_IN_PROGRESS (-110) // 실제 값은 환경에 맞게 수정
//#endif
//
//
//int setLiveViewSize(Camera *camera, GPContext *context, const char *size) {
//    CameraWidget *config = nullptr, *sizeWidget = nullptr;
//    int ret = gp_camera_get_config(camera, &config, context);
//    if (ret < GP_OK) {
//        LOGE("get_config 실패: %s", gp_result_as_string(ret));
//        return ret;
//    }
//
//    ret = gp_widget_get_child_by_name(config, "liveviewsize", &sizeWidget);
//    if (ret < GP_OK) {
//        LOGE("liveviewsize 위젯 없음: %s", gp_result_as_string(ret));
//        gp_widget_free(config);
//        return ret;
//    }
//
//    ret = gp_widget_set_value(sizeWidget, size);
//    if (ret < GP_OK) {
//        LOGE("라이브뷰 사이즈 설정 실패: %s", gp_result_as_string(ret));
//    } else {
//        ret = gp_camera_set_config(camera, config, context);
//        if (ret < GP_OK) {
//            LOGE("config 적용 실패: %s", gp_result_as_string(ret));
//        } else {
//            LOGD("라이브뷰 사이즈 설정 성공: %s", size);
//        }
//    }
//
//    gp_widget_free(config);
//    return ret;
//}
//// 메시지 콜백 함수: libgphoto2의 일반 메시지를 처리
//void message_callback(GPContext *context, const char *str, void *data) {
//    LOGE("libgphoto2 message: %s", str);
//}
//
//// 에러 콜백 함수: libgphoto2의 에러 메시지를 처리
//void error_callback(GPContext *context, const char *str, void *data) {
//    LOGE("libgphoto2 error: %s", str);
//}
//// 자동 초점 설정 함수 (카메라 지원 시)
//int setAutoFocus(Camera *camera, GPContext *context) {
//    CameraWidget *config = nullptr;
//    int ret = gp_camera_get_config(camera, &config, context);
//    if (ret < GP_OK) {
//        LOGE("카메라 설정 불러오기 실패: %s", gp_result_as_string(ret));
//        return ret;
//    }
//
//    // 카메라에 따라 자동 초점 위젯 이름은 다를 수 있음. 예시로 "autofocus" 사용.
//    CameraWidget *af_widget = nullptr;
//    ret = gp_widget_get_child_by_name(config, "autofocus", &af_widget);
//    if (ret < GP_OK) {
//        LOGE("자동 초점 위젯 검색 실패: %s", gp_result_as_string(ret));
//        gp_widget_free(config);
//        return ret;
//    }
//
//    ret = gp_widget_set_value(af_widget, "Auto");
//    if (ret < GP_OK) {
//        LOGE("자동 초점 설정 실패: %s", gp_result_as_string(ret));
//    } else {
//        LOGD("자동 초점 설정 성공");
//    }
//    gp_widget_free(config);
//    return ret;
//}
//
//
//int setLiveViewAutoFocus(Camera *camera, GPContext *context) {
//    CameraWidget *config = nullptr, *afWidget = nullptr;
//    int ret;
//
//    // 카메라의 전체 설정 트리 가져오기
//    ret = gp_camera_get_config(camera, &config, context);
//    if (ret < GP_OK) {
//        LOGE("카메라 설정 불러오기 실패: %s", gp_result_as_string(ret));
//        return ret;
//    }
//    LOGE("카메라 설정 불러오기 성공: %s", gp_result_as_string(ret));
//    // "Live View AF Focus" 위젯 검색 (대소문자와 띄어쓰기에 주의)
//    ret = gp_widget_get_child_by_label(config, "Live View AF Focus", &afWidget);
//    if (ret < GP_OK) {
//        // 없으면 이름으로도 시도 (혹은 다른 키워드로)
//        ret = gp_widget_get_child_by_name(config, "Live View AF Focus", &afWidget);
//        if (ret < GP_OK) {
//            LOGE("Live View AF Focus 위젯 검색 실패: %s", gp_result_as_string(ret));
//            gp_widget_free(config);
//            return ret;
//        }
//    }
//
//    // 현재 위젯 타입과 값을 확인 (출력된 값: Enumeration [0,1,4])
//    // 자동 초점 활성화를 위해, 예를 들어 "1"로 설정합니다.
//    const char *newValue = "4";
//    ret = gp_widget_set_value(afWidget, newValue);
//    if (ret < GP_OK) {
//        LOGE("자동 초점 설정 실패: %s", gp_result_as_string(ret));
//    } else {
//        LOGD("자동 초점 설정 성공 (값: %s)", newValue);
//    }
//
//    // 변경된 설정을 카메라에 적용
//    ret = gp_camera_set_config(camera, config, context);
//    if (ret < GP_OK) {
//        LOGE("카메라 설정 적용 실패: %s", gp_result_as_string(ret));
//    }
//
//    gp_widget_free(config);
//    return ret;
//}
//
//// Helper: CameraWidgetType -> string
//static const char *widgetTypeToString(CameraWidgetType type) {
//    switch (type) {
//        case GP_WIDGET_WINDOW:
//            return "WINDOW";
//        case GP_WIDGET_SECTION:
//            return "SECTION";
//        case GP_WIDGET_TEXT:
//            return "TEXT";
//        case GP_WIDGET_RANGE:
//            return "RANGE";
//        case GP_WIDGET_TOGGLE:
//            return "TOGGLE";
//        case GP_WIDGET_RADIO:
//            return "RADIO";
//        case GP_WIDGET_MENU:
//            return "MENU";
//        case GP_WIDGET_BUTTON:
//            return "BUTTON";
//        default:
//            return "UNKNOWN";
//    }
//}
//
//static void callJavaPhotoCallback(JNIEnv *env, const char *path) {
//    jclass cls = env->GetObjectClass(gCallback);
//    jmethodID mid = env->GetMethodID(cls, "onPhotoCaptured", "(Ljava/lang/String;)V");
//    env->CallVoidMethod(gCallback, mid, env->NewStringUTF(path));
//}
//
//// Recursive printer
//static void printWidget(CameraWidget *widget, int depth, std::ostringstream &oss) {
//    const char *name = nullptr, *label = nullptr;
//    gp_widget_get_name(widget, &name);
//    gp_widget_get_label(widget, &label);
//    oss << std::string(depth * 2, ' ') << (name ? name : "?") << " (" << (label ? label : "?")
//        << ") — "
//        << widgetTypeToString((CameraWidgetType) 0);
//
//    CameraWidgetType type;
//    if (gp_widget_get_type(widget, &type) == GP_OK) {
//        oss << widgetTypeToString(type);
//        if (type == GP_WIDGET_RADIO || type == GP_WIDGET_MENU) {
//            oss << " [choices:";
//            int count = gp_widget_count_choices(widget);
//            for (int i = 0; i < count; i++) {
//                const char *choice = nullptr;
//                gp_widget_get_choice(widget, i, &choice);
//                oss << ' ' << (choice ? choice : "?");
//            }
//            oss << "]";
//        }
//    }
//    oss << '\n';
//
//    CameraWidget *child = nullptr;
//    for (int i = 0; gp_widget_get_child(widget, i, &child) == GP_OK; ++i) {
//        printWidget(child, depth + 1, oss);
//    }
//}
//
//extern "C" JNIEXPORT jstring JNICALL
//Java_com_inik_phototest2_CameraNative_listCameraCapabilities(JNIEnv *env, jobject) {
//    if (!camera) return env->NewStringUTF("Camera not initialized");
//
//    CameraWidget *config = nullptr;
//    int ret = gp_camera_get_config(camera, &config, context);
//    if (ret < GP_OK) {
//        return env->NewStringUTF(gp_result_as_string(ret));
//    }
//
//    std::ostringstream oss;
//    printWidget(config, 0, oss);
//    gp_widget_free(config);
//
//    return env->NewStringUTF(oss.str().c_str());
//}
//
//// JNI_OnLoad: JavaVM 및 gp_context 초기화
//extern "C" JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *vm, void *) {
//    gJvm = vm;
//    context = gp_context_new();
//    LOGD("JNI_OnLoad: gJvm = %p, gp_context_new 완료", gJvm);
//    return JNI_VERSION_1_6;
//}
//
////
//// --- 기존 카메라 기능들 ---
////
//
//extern "C" JNIEXPORT jstring JNICALL
//Java_com_inik_phototest2_CameraNative_testLibraryLoad(JNIEnv *env, jobject) {
//    LOGD("testLibraryLoad 호출");
//    GPPortInfoList *pil = nullptr;
//    int ret = gp_port_info_list_new(&pil);
//    if (ret < GP_OK) return env->NewStringUTF(gp_result_as_string(ret));
//    ret = gp_port_info_list_load(pil);
//    gp_port_info_list_free(pil);
//    LOGD("testLibraryLoad 결과: %s", gp_result_as_string(ret));
//    return env->NewStringUTF(ret >= GP_OK ? "OK" : gp_result_as_string(ret));
//}
//
//extern "C" JNIEXPORT jstring JNICALL
//Java_com_inik_phototest2_CameraNative_getLibGphoto2Version(JNIEnv *env, jobject) {
//    const char **v = gp_library_version(GP_VERSION_SHORT);
//    return env->NewStringUTF(v && v[0] ? v[0] : "Unknown");
//}
//
//extern "C" JNIEXPORT jstring JNICALL
//Java_com_inik_phototest2_CameraNative_getPortInfo(JNIEnv *env, jobject) {
//    LOGD("getPortInfo 호출");
//    GPPortInfoList *pil = nullptr;
//    gp_port_info_list_new(&pil);
//    int ret = gp_port_info_list_load(pil);
//    std::ostringstream oss;
//    int count = gp_port_info_list_count(pil);
//    for (int i = 0; i < count; i++) {
//        GPPortInfo info;
//        if (gp_port_info_list_get_info(pil, i, &info) == GP_OK) {
//            const char *name, *path;
//            gp_port_info_get_name(info, (char **) &name);
//            gp_port_info_get_path(info, (char **) &path);
//            oss << name << " @ " << path << "\n";
//        }
//    }
//    gp_port_info_list_free(pil);
//    LOGD("getPortInfo 결과:\n%s", oss.str().c_str());
//    return env->NewStringUTF(oss.str().c_str());
//}
//
//extern "C" JNIEXPORT jstring JNICALL
//Java_com_inik_phototest2_CameraNative_initCamera(JNIEnv *env, jobject) {
//    LOGD("initCamera 호출");
//    std::lock_guard<std::mutex> lock(mtx);
//    int ret = gp_camera_new(&camera);
//    if (ret < GP_OK) {
//        LOGE("gp_camera_new 실패: %s", gp_result_as_string(ret));
//        return env->NewStringUTF(gp_result_as_string(ret));
//    }
//    ret = gp_camera_init(camera, context);
//    LOGD("initCamera 결과: %s", gp_result_as_string(ret));
//    return env->NewStringUTF(gp_result_as_string(ret));
//}
//
//extern "C" JNIEXPORT jstring JNICALL
//Java_com_inik_phototest2_CameraNative_detectCamera(JNIEnv *env, jobject) {
//    LOGD("detectCamera 호출");
//    CameraList *cl = nullptr;
//    gp_list_new(&cl);
//    int ret = gp_camera_autodetect(cl, context);
//    int count = gp_list_count(cl);
//    std::ostringstream oss;
//    if (ret >= GP_OK && count > 0) {
//        for (int i = 0; i < count; i++) {
//            const char *name, *port;
//            gp_list_get_name(cl, i, &name);
//            gp_list_get_value(cl, i, &port);
//            oss << name << " @ " << port << "\n";
//        }
//    } else {
//        oss << "No camera detected";
//    }
//    gp_list_free(cl);
////    LOGD("detectCamera 결과:\n%s", oss.str().c_str());
//    return env->NewStringUTF(oss.str().c_str());
//}
//
//extern "C" JNIEXPORT jstring JNICALL
//Java_com_inik_phototest2_CameraNative_getCameraSummary(JNIEnv *env, jobject) {
//    LOGD("getCameraSummary 호출");
//    if (!camera) {
//        LOGE("camera 객체가 초기화되지 않았습니다.");
//        return env->NewStringUTF("Camera not initialized");
//    }
//    CameraText txt;
//    int ret = gp_camera_get_summary(camera, &txt, context);
//    if (ret < GP_OK) {
//        LOGE("gp_camera_get_summary 실패: %s", gp_result_as_string(ret));
//        return env->NewStringUTF(gp_result_as_string(ret));
//    }
//    LOGD("Summary 결과: %s", txt.text);
//    return env->NewStringUTF(txt.text);
//}
//
//extern "C" JNIEXPORT jboolean JNICALL
//Java_com_inik_phototest2_CameraNative_isCameraConnected(JNIEnv *env, jobject) {
//    LOGD("isCameraConnected 호출");
//    CameraList *cl = nullptr;
//    gp_list_new(&cl);
//    int ret = gp_camera_autodetect(cl, context);
//    int count = gp_list_count(cl);
//    gp_list_free(cl);
//    bool connected = (ret >= GP_OK && count > 0);
//    LOGD("isCameraConnected: %s", connected ? "true" : "false");
//    return connected;
//}
//
//extern "C" JNIEXPORT jint JNICALL
//Java_com_inik_phototest2_CameraNative_capturePhoto(JNIEnv *env, jobject, jstring) {
//    // CameraFilePath 생성
//    CameraFilePath cfp;
//    int ret = gp_camera_capture(camera, GP_CAPTURE_IMAGE, &cfp, context);
//    if (ret < GP_OK) {
//        LOGE("gp_camera_capture 실패: %s", gp_result_as_string(ret));
//        return ret;
//    }
//
//    // 파일 가져오기
//    CameraFile *file;
//    gp_file_new(&file);
//    int getRet = gp_camera_file_get(camera, cfp.folder, cfp.name, GP_FILE_TYPE_NORMAL, file, context);
//    if (getRet < GP_OK) {
//        LOGE("gp_camera_file_get 실패: %s", gp_result_as_string(getRet));
//        gp_file_free(file);
//        return getRet;
//    }
//
//    // 저장 경로 구성 (listenCameraEvents 방식)
//    char savePath[128];
//    snprintf(savePath, sizeof(savePath),
//             "/data/data/com.inik.phototest2/files/photo_%lld.jpg",
//             (long long)std::time(nullptr));
//
//    {
//        std::lock_guard<std::mutex> lock(cameraMutex);
//        CameraWidget *config = nullptr, *vf = nullptr;
//        int ret = gp_camera_get_config(camera, &config, context);
//        if (ret >= GP_OK &&
//            gp_widget_get_child_by_name(config, "controlmode", &vf) == GP_OK) {
//            int off = 0;
//            gp_widget_set_value(vf, &off);
//            gp_camera_set_config(camera, config, context);
//            LOGD("Remote Mode 해제: controlmode OFF");
//        }
//        gp_widget_free(config);
//    }
//
//    // 파일 저장
//    gp_file_save(file, savePath);
//    gp_file_free(file);
//    LOGD("capturePhoto 저장 완료: %s", savePath);
//
//
//    return ret;
//}
//
//extern "C" JNIEXPORT void JNICALL
//Java_com_inik_phototest2_CameraNative_capturePhotoAsync(JNIEnv *env, jobject, jobject cb) {
//    LOGD("capturePhotoAsync 호출");
//    jobject globalCb = env->NewGlobalRef(cb);
//    JavaVM *vm;
//    env->GetJavaVM(&vm);
//    std::thread([vm, globalCb]() {
//        JNIEnv *threadEnv;
//        vm->AttachCurrentThread(&threadEnv, nullptr);
//        jstring path = threadEnv->NewStringUTF("/data/data/com.inik.phototest2/files/photo.jpg");
//        jint result = Java_com_inik_phototest2_CameraNative_capturePhoto(threadEnv, nullptr, path);
//        jclass cls = threadEnv->GetObjectClass(globalCb);
//        if (result >= GP_OK) {
//            jmethodID m = threadEnv->GetMethodID(cls, "onPhotoCaptured", "(Ljava/lang/String;)V");
//            threadEnv->CallVoidMethod(globalCb, m, path);
//        } else {
//            jmethodID m = threadEnv->GetMethodID(cls, "onCaptureFailed", "(I)V");
//            threadEnv->CallVoidMethod(globalCb, m, result);
//        }
//        threadEnv->DeleteGlobalRef(globalCb);
//        vm->DetachCurrentThread();
//    }).detach();
//}
//extern "C" JNIEXPORT jint JNICALL
//Java_com_inik_phototest2_CameraNative_initCameraWithFd(
//        JNIEnv *env, jobject, jint fd, jstring libDir_) {
//
//    const char *libDir = env->GetStringUTFChars(libDir_, nullptr);
//    std::lock_guard<std::mutex> lock(mtx);
//    LOGD("initCameraWithFd 시작: fd=%d, libDir=%s", fd, libDir);
//
//    setenv("CAMLIBS", libDir, 1);
//    setenv("IOLIBS", libDir, 1);
//    setenv("CAMLIBS_PREFIX", "libgphoto2_camlib_", 1);
//    setenv("IOLIBS_PREFIX", "libgphoto2_port_iolib_", 1);
//
//    // Cleanup 기존 인스턴스
//    if (camera) {
//        LOGD("카메라 인스턴스 있음");
//        gp_camera_exit(camera, context);
//        gp_camera_free(camera);
//        camera = nullptr;
//    }
//
//    int ret = gp_port_usb_set_sys_device(fd);
//    if (ret < GP_OK) goto cleanup;
//
//    // 반복 초기화 (최대 3회)
//    for (int i = 0; i < 3; ++i) {
//        ret = gp_camera_new(&camera);
//        if (ret < GP_OK) goto cleanup;
//
//        ret = gp_camera_init(camera, context);
//        if (ret == GP_OK) {
//            LOGD("gp_camera_init 성공 (retry #%d)", i + 1);
//            break;
//        }
//        LOGE("gp_camera_init 실패 (retry #%d ret=%d): %s", i + 1, ret, gp_result_as_string(ret));
//        gp_camera_free(camera);
//        camera = nullptr;
//        std::this_thread::sleep_for(std::chrono::milliseconds(500));
//    }
//
//    cleanup:
//    if (ret < GP_OK) {
//        LOGE("초기화 최종 실패 (ret=%d): %s", ret, gp_result_as_string(ret));
//        if (camera) {
//            gp_camera_exit(camera, context);
//            gp_camera_free(camera);
//            camera = nullptr;
//        }
//    }
//
//    env->ReleaseStringUTFChars(libDir_, libDir);
//    return ret;
//}
//
////extern "C" JNIEXPORT jint JNICALL
////Java_com_inik_phototest2_CameraNative_initCameraWithFd(
////        JNIEnv *env, jobject, jint fd, jstring libDir_) {
////
////    const char *libDir = env->GetStringUTFChars(libDir_, nullptr);
////    std::lock_guard<std::mutex> lock(mtx);
////    LOGD("initCameraWithFd 시작: fd=%d, libDir=%s", fd, libDir);
////
////    setenv("CAMLIBS", libDir, 1);
////    setenv("IOLIBS", libDir, 1);
////    setenv("CAMLIBS_PREFIX", "libgphoto2_camlib_", 1);
////    setenv("IOLIBS_PREFIX", "libgphoto2_port_iolib_", 1);
////    LOGD("환경변수 설정 완료");
////
////    int ret = gp_port_usb_set_sys_device(fd);
////    if (ret < GP_OK) {
////        LOGE("gp_port_usb_set_sys_device 실패: %s", gp_result_as_string(ret));
////        env->ReleaseStringUTFChars(libDir_, libDir);
////        return ret;
////    }
////    LOGD("gp_port_usb_set_sys_device 성공");
////
////    ret = gp_camera_new(&camera);
////    if (ret < GP_OK) {
////        LOGE("gp_camera_new 실패: %s", gp_result_as_string(ret));
////        env->ReleaseStringUTFChars(libDir_, libDir);
////        return ret;
////    }
////    LOGD("gp_camera_new 성공");
////
////    ret = gp_camera_init(camera, context);
////    if (ret < GP_OK) {
////        char *err = const_cast<char *>(gp_result_as_string(ret));
////        LOGE("gp_camera_init 실패 (ret=%d): %s", ret, err);
////        LOGE("gp_camera_init 실패: %s", gp_result_as_string(ret));
////    }
////    else
////        LOGD("gp_camera_init 성공");
////
////    env->ReleaseStringUTFChars(libDir_, libDir);
////    return ret;
////}
//
//extern "C" JNIEXPORT void JNICALL
//Java_com_inik_phototest2_CameraNative_closeCamera(JNIEnv *, jobject) {
//    LOGD("closeCamera 호출");
//    if (camera) {
//        gp_camera_exit(camera, context);
//        gp_camera_free(camera);
//        camera = nullptr;
//    }
//    if (context) {
//        gp_context_unref(context);
//        context = nullptr;
//    }
//    LOGD("closeCamera 완료");
//}
//
////
//// --- 이벤트 리스너 기능 ---
////
//extern "C" JNIEXPORT void JNICALL
//Java_com_inik_phototest2_CameraNative_listenCameraEvents(
//        JNIEnv *env, jobject, jobject callback) {
//
//    // 중복 실행 방지
//    if (eventListenerRunning.load()) {
//        LOGD("startListenCameraEvents: 이미 이벤트 리스너가 실행 중입니다.");
//        return;
//    }
//
//    jobject globalCb = env->NewGlobalRef(callback);
//    JavaVM *vm;
//    env->GetJavaVM(&vm);
//
//    eventListenerRunning.store(true);
//    eventListenerThread = std::thread([vm, globalCb]() {
//        JNIEnv *threadEnv;
//        if (vm->AttachCurrentThread(&threadEnv, nullptr) != JNI_OK) {
//            LOGE("eventListenerLoop: AttachCurrentThread 실패");
//            return;
//        }
//        LOGD("eventListenerLoop: AttachCurrentThread 성공");
//        static std::atomic<int> photoCounter{0};
//
//        while (eventListenerRunning.load()) {
//            { // 뮤텍스 잠금
//                std::lock_guard<std::mutex> lock(mtx);
//                if (!camera) {
//                    LOGE("Camera 객체 없음, 이벤트 루프 종료");
//                    break;
//                }
//
//                CameraEventType type;
//                void *data = nullptr;
//                int ret = gp_camera_wait_for_event(camera, 5000, &type, &data, context);
//                if (!eventListenerRunning.load()) break;
//                if (ret != GP_OK) {
//                    LOGE("gp_camera_wait_for_event 실패: %s", gp_result_as_string(ret));
//                    std::this_thread::sleep_for(std::chrono::milliseconds(500));
//                    continue;
//                }
//
//                if (type == GP_EVENT_FILE_ADDED) {
//                    CameraFilePath *cfp = static_cast<CameraFilePath *>(data);
//                    LOGD("새로운 파일 추가됨: %s/%s", cfp->folder, cfp->name);
//
//                    char pathBuf[128];
//                    {
//                        // 밀리초 단위 타임스탬프와 카운터를 사용하여 고유 파일명 생성
//                        auto now = std::chrono::system_clock::now();
//                        auto nowMs = std::chrono::time_point_cast<std::chrono::milliseconds>(now);
//                        long long millis = nowMs.time_since_epoch().count();
//                        int count = photoCounter.fetch_add(1);
//                        snprintf(pathBuf, sizeof(pathBuf),
////                                 "/data/data/com.inik.phototest2/files/photo_%lld_%d.jpg",
//                                 "/data/data/com.inik.phototest2/files/photo_%lld_%d.jpg",
//                                 millis, count);
//                    }
//
//                    CameraFile *file;
//                    gp_file_new(&file);
//                    int getRet = -1;
//                    for (int i = 0; i < 5; ++i) {
//                        getRet = gp_camera_file_get(camera, cfp->folder, cfp->name,
//                                                    GP_FILE_TYPE_NORMAL, file, context);
//                        if (getRet >= GP_OK) break;
//                        LOGD("파일 준비 대기 중... (%d)", i + 1);
//                        std::this_thread::sleep_for(std::chrono::milliseconds(300));
//                    }
//
//                    if (getRet >= GP_OK) {
//                        gp_file_save(file, pathBuf);
//                        LOGD("사진 저장 완료: %s", file);
//                        jclass cls = threadEnv->GetObjectClass(globalCb);
//                        jmethodID m = threadEnv->GetMethodID(cls, "onPhotoCaptured",
//                                                             "(Ljava/lang/String;)V");
//                        jstring pathStr = threadEnv->NewStringUTF(pathBuf);
//                        threadEnv->CallVoidMethod(globalCb, m, pathStr);
//                    } else {
//                        LOGE("사진 가져오기 실패: %s", gp_result_as_string(getRet));
//                        jclass cls = threadEnv->GetObjectClass(globalCb);
//                        jmethodID m = threadEnv->GetMethodID(cls, "onCaptureFailed", "(I)V");
//                        threadEnv->CallVoidMethod(globalCb, m, getRet);
//                    }
//                    gp_file_free(file);
//                } else if (type == GP_EVENT_CAPTURE_COMPLETE) {
//                    LOGD("촬영 완료 이벤트 (CAPTURE_COMPLETE) 수신됨 - 무시");
//                }
//            }
//            std::unique_lock<std::mutex> lk(eventCvMtx);
//            eventCv.wait_for(lk, std::chrono::milliseconds(100),
//                             [] { return !eventListenerRunning.load(); });
//        }
//        threadEnv->DeleteGlobalRef(globalCb);
//        vm->DetachCurrentThread();
//        LOGD("eventListenerLoop 종료 완료");
//    });
//}
//
//extern "C" JNIEXPORT void JNICALL
//Java_com_inik_phototest2_CameraNative_stopListenCameraEvents(JNIEnv *env, jobject) {
//    eventListenerRunning.store(false);
//    eventCv.notify_all();
//
//    // join 작업을 **별도** 쓰레드로 옮겨 메인 블록 방지 + double-join 방지
//    std::thread([&]() {
//        if (eventListenerThread.joinable()) {
//            eventListenerThread.join();
//            LOGD("eventListenerThread 정상 종료");
//        }
//    }).detach();
//
//    LOGD("stopListenCameraEvents: 이벤트 리스너 종료 요청 완료");
//}
//
//static void liveViewLoop() {
//    JNIEnv *env;
//    gJvm->AttachCurrentThread(&env, nullptr);
//    CameraFile *file = nullptr;
//    gp_file_new(&file);
//
//    while (liveViewRunning.load()) {
//        {
//            std::lock_guard<std::mutex> lock(cameraMutex);
//            gp_camera_capture_preview(camera, file, context);
//
//            const char *data;
//            unsigned long size;
//            gp_file_get_data_and_size(file, &data, &size);
////            jbyteArray arr = env->NewByteArray(size);
////            env->SetByteArrayRegion(arr, 0, size, (jbyte *) data);
//            jobject byteBuffer = env->NewDirectByteBuffer((void*)data, size);
//
//
//            jclass cls = env->GetObjectClass(gCallback);
//            jmethodID mid = env->GetMethodID(cls, "onLiveViewFrame", "(Ljava/nio/ByteBuffer;)V");
//            env->CallVoidMethod(gCallback, mid, byteBuffer);
//            env->DeleteLocalRef(byteBuffer);
//
//            if (captureRequested.exchange(false)) {
//                CameraFilePath cfp;
//                if (gp_camera_capture(camera, GP_CAPTURE_IMAGE, &cfp, context) >= GP_OK) {
//                    CameraFile *photoFile;
//                    gp_file_new(&photoFile);
//                    gp_camera_file_get(camera, cfp.folder, cfp.name, GP_FILE_TYPE_NORMAL, photoFile,
//                                       context);
//                    char path[128];
//                    snprintf(path, sizeof(path),
//                             "/data/data/com.inik.phototest2/files/photo_%lld.jpg",
//                             (long long) time(nullptr));
//                    gp_file_save(photoFile, path);
//                    gp_file_free(photoFile);
//                    callJavaPhotoCallback(env, path);
//                }
//            }
//        }
//        gp_file_free(file);
//        gp_file_new(&file);
//
//        std::this_thread::sleep_for(std::chrono::milliseconds(42));
//    }
//    gp_file_free(file);
//    gJvm->DetachCurrentThread();
//}
//
//
//extern "C" JNIEXPORT void JNICALL
//Java_com_inik_phototest2_CameraNative_startLiveView(JNIEnv *env, jobject, jobject callback) {
//    if (liveViewRunning.load()) return;
//    gCallback = env->NewGlobalRef(callback);
//
//    std::thread([](){
//        std::lock_guard<std::mutex> lock(cameraMutex);
//        GPContext *ctx = context; // 전역 변수 사용 시 thread-safe 확인
//        Camera *cam = nullptr;
//
//        gp_camera_new(&cam);
//        gp_camera_init(cam, ctx);
//
//        // autofocusdrive 설정
//        CameraWidget *config = nullptr, *widget = nullptr;
//        if (gp_camera_get_config(cam, &config, ctx) == GP_OK &&
//            gp_widget_get_child_by_name(config, "autofocusdrive", &widget) == GP_OK) {
//            int val = 1;
//            gp_widget_set_value(widget, &val);
//            gp_camera_set_config(cam, config, ctx);
//        }
//
//        setLiveViewSize(cam, ctx, "XGA");
//        liveViewRunning.store(true);
//        std::thread(liveViewLoop).detach();
//
//        LOGD("LiveView started (background thread)");
//    }).detach();
//
//    LOGD("startLiveView() returned immediately");
//}
///**
// * stopLiveView: 라이브뷰 종료
// */
//
//extern "C" JNIEXPORT void JNICALL
//Java_com_inik_phototest2_CameraNative_stopLiveView(JNIEnv *env, jobject) {
//
//
//    // 1️⃣ LiveView 루프 종료
//    liveViewRunning.store(false);
//    cv.notify_all();
//    if (liveViewThread.joinable()) liveViewThread.join();
//
//    // 2️⃣ PTP Remote Mode 해제: “controlmode” OFF
//    {
//        std::lock_guard<std::mutex> lock(cameraMutex);
//        CameraWidget *config = nullptr, *vf = nullptr;
//        int ret = gp_camera_get_config(camera, &config, context);
//        if (ret >= GP_OK &&
//            gp_widget_get_child_by_name(config, "controlmode", &vf) == GP_OK) {
//            int off = 0;
//            gp_widget_set_value(vf, &off);
//            gp_camera_set_config(camera, config, context);
//            LOGD("Remote Mode 해제: controlmode OFF");
//        }
//        gp_widget_free(config);
//    }
//
//    // 3️⃣ Callback 해제만 — 카메라 세션은 유지
//    if (gCallback) {
//        env->DeleteGlobalRef(gCallback);
//        gCallback = nullptr;
//    }
//    LOGD("LiveView stopped (Camera remains connected)");
//}
//
//extern "C" JNIEXPORT void JNICALL
//Java_com_inik_phototest2_CameraNative_requestCapture(JNIEnv *env, jobject) {
//    captureRequested.store(true);
//}
//
//extern "C" JNIEXPORT jstring JNICALL
//Java_com_inik_phototest2_CameraNative_cameraAutoDetect(JNIEnv *env, jobject obj) {
//    // GPContext 생성 및 콜백 함수 설정
//    GPContext *context = gp_context_new();
//    gp_context_set_message_func(context, message_callback, NULL);
//    gp_context_set_error_func(context, error_callback, NULL);
//
//    // 예제: 카메라 자동 감지 호출
//    CameraList *list;
//    int ret = gp_list_new(&list);
//    if(ret < GP_OK) {
//        return env->NewStringUTF("Failed to create camera list");
//    }
//    ret = gp_camera_autodetect(list, context);
//    if(ret < GP_OK) {
//        gp_list_free(list);
//        return env->NewStringUTF( "Camera autodetect failed");
//    }
//
//    // 결과 문자열 생성 (예시)
//    int count = gp_list_count(list);
//    char result[1024] = {0};
//    snprintf(result, sizeof(result), "Detected %d cameras\n", count);
//
//    for (int i = 0; i < count; i++) {
//        const char *name, *port;
//        gp_list_get_name(list, i, &name);
//        gp_list_get_value(list, i, &port);
//        char buffer[256];
//        snprintf(buffer, sizeof(buffer), "Camera: %s, Port: %s\n", name, port);
//        strncat(result, buffer, sizeof(result) - strlen(result) - 1);
//    }
//    LOGE("%s", result);
//
//
//    gp_list_free(list);
//    return env->NewStringUTF(result);
//}

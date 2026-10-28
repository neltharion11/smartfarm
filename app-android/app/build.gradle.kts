import java.util.Properties

plugins {
    alias(libs.plugins.android.application)
    // ⚠️ 不要再加 `org.jetbrains.kotlin.android`：
    // AGP 9.0 起**内置了 Kotlin 支持**，重复应用会直接报错：
    //   "The 'org.jetbrains.kotlin.android' plugin is no longer required
    //    for Kotlin support since AGP 9.0."
    // 参考 https://developer.android.com/build/migrate-to-built-in-kotlin
    alias(libs.plugins.kotlin.compose)
}

// APP 的本地配置只从自己的工程目录读取；模板见 mqtt.properties.example。
// 文件缺失时仍可编译，MQTT 界面会提示未配置。
val mqttProps = Properties().apply {
    val configFile = rootProject.file("mqtt.properties")
    if (configFile.isFile) configFile.reader(Charsets.UTF_8).use { load(it) }
}
fun mqttProp(name: String): String = mqttProps.getProperty(name, "")

// 生成 Java 字符串字面量必须转义：密码里若含 " 或 \ 会破坏生成的 BuildConfig.java
fun javaStr(s: String): String = "\"" + s.replace("\\", "\\\\").replace("\"", "\\\"")
    .replace("\r", "\\r").replace("\n", "\\n") + "\""

android {
    namespace = "com.smartfarm.app"
    // 本机 SDK 已有 platform android-37.0
    compileSdk = 37

    defaultConfig {
        applicationId = "com.smartfarm.app"
        // minSdk 26 (Android 8.0)：BLE 与 Compose 都稳妥支持
        minSdk = 26
        // targetSdk 36 (Android 16)：比 compileSdk 略低，兼容面更广
        targetSdk = 36
        versionCode = 2
        versionName = "0.2.0"

        // ⚠️ 这些值会进入 APK。APK 因此是**含密产物**，不要对外分发。
        buildConfigField("String", "MQTT_URI", javaStr(mqttProp("mqtt.uri")))
        buildConfigField("String", "MQTT_WS_URI", javaStr(mqttProp("mqtt.wsUri")))
        buildConfigField("String", "MQTT_USER", javaStr(mqttProp("mqtt.user")))
        buildConfigField("String", "MQTT_PASS", javaStr(mqttProp("mqtt.password")))
        buildConfigField("String", "TOPIC_ROOT", javaStr(mqttProp("mqtt.topicRoot")))
    }

    buildTypes {
        release {
            isMinifyEnabled = false
        }
    }

    // ⚠️ 不要写 jvmToolchain(17)：本机只有 Studio 自带的 JDK 25，
    // 声明 toolchain 会让 Gradle 去找一个并不存在的 JDK 17 而失败。
    // 用 JDK 25 编译、产出 17 的字节码即可（javac/kotlinc 都支持向低版本 target）。
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    buildFeatures {
        compose = true
        // AGP 8.0 起 BuildConfig 默认不再生成；本项目要用它把凭据与主题传进 APP
        buildConfig = true
    }
}

// 注：Kotlin 编译器选项由 AGP 内置的 Kotlin 支持接管，
// 这里不再用 `kotlin { compilerOptions { ... } }`（那是独立 Kotlin 插件的 DSL）。

dependencies {
    implementation(libs.androidx.core.ktx)
    implementation(libs.androidx.lifecycle.runtime.ktx)
    implementation(libs.androidx.activity.compose)

    implementation(platform(libs.androidx.compose.bom))
    implementation(libs.androidx.ui)
    implementation(libs.androidx.ui.graphics)
    implementation(libs.androidx.ui.tooling.preview)
    implementation(libs.androidx.material3)

    debugImplementation(libs.androidx.ui.tooling)

    // C4：MQTT over WebSocket
    implementation(libs.paho.mqttv3)

    testImplementation(libs.junit)
    testImplementation(libs.orgjson)
}

// 根工程：只声明插件，不在这里应用（各模块自行 apply）
// 注意 AGP 9.0 起内置 Kotlin 支持，**不再**声明 kotlin.android 插件。
plugins {
    alias(libs.plugins.android.application) apply false
    alias(libs.plugins.kotlin.compose) apply false
}

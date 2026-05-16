package com.example.myapplication; // **请确保这里的包名正确**

import androidx.appcompat.app.AppCompatActivity;
import android.os.Bundle;
import android.widget.TextView;
import android.util.Log;

public class MainActivity extends AppCompatActivity {

    private static final String TAG = "DobbyApp";

    static {
        System.loadLibrary("jhhook");

        final Thread.UncaughtExceptionHandler delegate = Thread.getDefaultUncaughtExceptionHandler();
        Thread.setDefaultUncaughtExceptionHandler((thread, ex) -> {
            Log.e("CrashTrace", "--- UncaughtException thread=" + thread.getName()
                    + " id=" + thread.getId() + " pid=" + android.os.Process.myPid() + " ---", ex);
            Log.e("CrashTrace", Log.getStackTraceString(ex));
            if (delegate != null)
                delegate.uncaughtException(thread, ex);
        });
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_main);

        TextView tv = findViewById(R.id.sample_text);

        // 1. 调用 stringFromJNI() 触发所有的 Dobby Hook 逻辑
        String logInfo = stringFromJNI();
        Log.d(TAG, "JNI Log Info:\n" + logInfo);

        // 2. 调用 getDobbyTestResult() 获取最终的 int 结果 (0 或 100)
        int finalResult = getDobbyTestResult();

        String displayMessage;

        if (finalResult == 100) {
            displayMessage = "Hook SUCCESS! Result: 100";
        } else if (finalResult == 0) {
            displayMessage = "Hook FAILED! Original Result: 0 (Check Logcat for status)";
        } else {
            // 如果 Hook 被跳过，或者返回其他非 0/100 的值，通常不应该发生
            displayMessage = "Unexpected Result: " + finalResult;
        }

        // 显示结果
        tv.setText(displayMessage);
    }

    // 声明 JNI 方法 (触发 Hook)
    public native String stringFromJNI();

    // 声明 JNI 方法 (获取最终结果)
    public native int getDobbyTestResult();
}
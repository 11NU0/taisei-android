package org.taisei_project;

import android.os.Bundle;
import android.view.WindowManager;

import org.libsdl.app.SDLActivity;

public class TaiseiActivity extends SDLActivity {

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
    }

    @Override
    protected String[] getLibraries() {
        return new String[] {
            "c++_shared",
            "taisei",
        };
    }

    @Override
    protected String getMainFunction() {
        return "SDL_main";
    }

    @Override
    protected String[] getArguments() {
        return new String[0];
    }
}

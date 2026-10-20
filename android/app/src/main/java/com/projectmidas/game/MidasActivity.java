package com.projectmidas.game;

import org.libsdl.app.SDLActivity;

/**
 * The game's activity: SDL's, unchanged. SDLActivity loads libSDL2.so and
 * libmain.so and calls SDL_main (port/src/android_pc.c). A subclass of our
 * own keeps the manifest's component name stable if this ever needs to
 * override anything.
 */
public class MidasActivity extends SDLActivity {
}

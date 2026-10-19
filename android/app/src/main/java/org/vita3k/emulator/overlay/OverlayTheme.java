package org.vita3k.emulator.overlay;

import android.content.ContentResolver;
import android.content.Context;
import android.database.Cursor;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.net.Uri;
import android.provider.OpenableColumns;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.Locale;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;

/**
 * Overlay button themes. A theme is a zip of PNG files named like the button drawables
 * (button_cross.png, button_cross_pressed.png, ...). Files missing from the theme fall back to the built-in art.
 */
public final class OverlayTheme {
  private static final String PREFS = "overlay_theme";
  private static final String KEY_ACTIVE = "active";

  private OverlayTheme() {
  }

  public static String getActive(Context context) {
    return context.getSharedPreferences(PREFS, Context.MODE_PRIVATE).getString(KEY_ACTIVE, "");
  }

  public static void setActive(Context context, String name) {
    context.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
            .edit().putString(KEY_ACTIVE, name == null ? "" : name).apply();
  }

  private static File themesRoot(Context context) {
    return new File(context.getFilesDir(), "overlay_themes");
  }

  /** Returns the themed PNG for this file name, or null when the active theme does not provide it. */
  public static Bitmap decodeTheme(Context context, String fileName) {
    final String active = getActive(context);
    if (active.isEmpty())
      return null;
    final File file = new File(new File(themesRoot(context), active), fileName);
    if (!file.isFile())
      return null;
    try {
      return BitmapFactory.decodeFile(file.getAbsolutePath());
    } catch (Throwable error) {
      return null;
    }
  }

  /** Themed bitmap for a button drawable, falling back to the built-in resource. */
  public static Bitmap decodeButton(Context context, int resId) {
    final String entry = context.getResources().getResourceEntryName(resId);
    final Bitmap themed = decodeTheme(context, entry + ".png");
    return themed != null ? themed : BitmapFactory.decodeResource(context.getResources(), resId);
  }

  /** Imports a theme zip chosen by the user and makes it active. Returns the theme name, or null on failure. */
  public static String importZip(Context context, Uri uri) {
    final ContentResolver resolver = context.getContentResolver();
    String name = sanitize(displayName(resolver, uri));
    if (name.isEmpty())
      name = "theme-" + System.currentTimeMillis();

    final File root = themesRoot(context);
    final File tmp = new File(root, name + ".tmp");
    final File target = new File(root, name);
    deleteRecursively(tmp);
    if (!tmp.mkdirs())
      return null;

    int extracted = 0;
    try (InputStream raw = resolver.openInputStream(uri)) {
      if (raw == null) {
        deleteRecursively(tmp);
        return null;
      }
      try (ZipInputStream zip = new ZipInputStream(raw)) {
        ZipEntry entry;
        while ((entry = zip.getNextEntry()) != null) {
          if (entry.isDirectory())
            continue;
          final String fileName = new File(entry.getName()).getName();
          if (!fileName.toLowerCase(Locale.ROOT).endsWith(".png"))
            continue;
          try (OutputStream out = new FileOutputStream(new File(tmp, fileName))) {
            final byte[] buffer = new byte[8192];
            int read;
            while ((read = zip.read(buffer)) > 0)
              out.write(buffer, 0, read);
          }
          extracted++;
        }
      }
    } catch (IOException error) {
      deleteRecursively(tmp);
      return null;
    }

    if (extracted == 0) {
      deleteRecursively(tmp);
      return null;
    }
    deleteRecursively(target);
    if (!tmp.renameTo(target)) {
      deleteRecursively(tmp);
      return null;
    }
    setActive(context, name);
    return name;
  }

  private static String displayName(ContentResolver resolver, Uri uri) {
    try (Cursor cursor = resolver.query(uri, new String[]{OpenableColumns.DISPLAY_NAME}, null, null, null)) {
      if (cursor != null && cursor.moveToFirst()) {
        final String name = cursor.getString(0);
        if (name != null)
          return name.replaceFirst("(?i)\\.zip$", "");
      }
    } catch (Throwable ignored) {
    }
    return "";
  }

  private static String sanitize(String name) {
    final String cleaned = name.replaceAll("[^A-Za-z0-9_-]", "_");
    return cleaned.length() > 40 ? cleaned.substring(0, 40) : cleaned;
  }

  private static void deleteRecursively(File file) {
    if (file == null || !file.exists())
      return;
    final File[] children = file.listFiles();
    if (children != null) {
      for (File child : children)
        deleteRecursively(child);
    }
    file.delete();
  }

  public static boolean isHidden(Context context, int legacyId) {
    return context.getSharedPreferences(PREFS, Context.MODE_PRIVATE).getBoolean("hide_" + legacyId, false);
  }

  public static void setHidden(Context context, int legacyId, boolean hidden) {
    context.getSharedPreferences(PREFS, Context.MODE_PRIVATE).edit().putBoolean("hide_" + legacyId, hidden).apply();
  }

  /** Per-button scale multiplier, 0.5..1.5 (1.0 = default size). */
  public static float getScale(Context context, int legacyId) {
    final float value = context.getSharedPreferences(PREFS, Context.MODE_PRIVATE).getFloat("scale_" + legacyId, 1.0f);
    return Math.max(0.5f, Math.min(1.5f, value));
  }

  public static void setScale(Context context, int legacyId, float scale) {
    context.getSharedPreferences(PREFS, Context.MODE_PRIVATE).edit().putFloat("scale_" + legacyId, scale).apply();
  }

  private static volatile Runnable sChangeListener;

  /** Set by the overlay so per-button edits rebuild it without a global scale change. */
  public static void setChangeListener(Runnable listener) {
    sChangeListener = listener;
  }

  public static void notifyChanged() {
    final Runnable listener = sChangeListener;
    if (listener != null)
      listener.run();
  }
}

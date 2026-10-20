package org.qtproject.example.Dvrqt;

import android.graphics.Bitmap;
import android.media.MediaMetadataRetriever;
import java.io.FileOutputStream;

// Generates a 320x180 JPEG thumbnail from a video file.
// MediaMetadataRetriever = the standard Android way (Gallery, players).
// It does NOT decode the whole file: it seeks to a sync sample and
// decodes exactly one frame. That is why it is fast.
public class ThumbHelper {
    public static boolean make(String video, String jpg, long atUs) {
        MediaMetadataRetriever r = new MediaMetadataRetriever();
        try {
            r.setDataSource(video);
            Bitmap b = r.getFrameAtTime(atUs,
                    MediaMetadataRetriever.OPTION_CLOSEST_SYNC);
            if (b == null) return false;
            Bitmap s = Bitmap.createScaledBitmap(b, 320, 180, true);
            FileOutputStream f = new FileOutputStream(jpg);
            s.compress(Bitmap.CompressFormat.JPEG, 80, f);
            f.close();
            return true;
        } catch (Throwable t) {
            return false;
        } finally {
            try { r.release(); } catch (Throwable ignored) {}
        }
    }
}
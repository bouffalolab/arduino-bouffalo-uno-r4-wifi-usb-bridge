package com.example.phonematrix;

import android.content.res.Resources;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.Canvas;
import android.graphics.ColorFilter;
import android.graphics.Paint;
import android.graphics.PixelFormat;
import android.graphics.Rect;
import android.graphics.drawable.Drawable;

/** Bundled pixel-art controls, rendered at an integer scale with sharp pixel edges. */
public final class ActionIcon extends Drawable {
    private final Paint paint = new Paint();
    private final Bitmap bitmap;
    private final Rect destination = new Rect();

    public ActionIcon(Resources resources, String action) {
        int resource;
        switch(action) {
            case "Heart": resource=R.drawable.pixel_heart; break;
            case "Smile": resource=R.drawable.pixel_smile; break;
            case "Checker": resource=R.drawable.pixel_grid; break;
            case "Clear": resource=R.drawable.pixel_clear; break;
            case "Stop demo": resource=R.drawable.pixel_stop; break;
            default: resource=R.drawable.pixel_play; break;
        }
        BitmapFactory.Options options = new BitmapFactory.Options();
        options.inScaled = false;
        bitmap = BitmapFactory.decodeResource(resources, resource, options);
        paint.setFilterBitmap(false);
        paint.setAntiAlias(false);
    }
    @Override public void draw(Canvas canvas) {
        int scale=Math.min(getBounds().width()/bitmap.getWidth(),getBounds().height()/bitmap.getHeight());
        if (scale < 1) return;
        int width=bitmap.getWidth()*scale,height=bitmap.getHeight()*scale;
        int left=getBounds().centerX()-width/2,top=getBounds().centerY()-height/2;
        destination.set(left,top,left+width,top+height);
        canvas.drawBitmap(bitmap,null,destination,paint);
    }
    @Override public void setAlpha(int alpha){paint.setAlpha(alpha);invalidateSelf();}
    @Override public void setColorFilter(ColorFilter filter){paint.setColorFilter(filter);invalidateSelf();}
    @Override public int getOpacity(){return PixelFormat.TRANSLUCENT;}
}

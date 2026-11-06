package com.example.phonematrix;

import android.content.Context;
import android.util.AttributeSet;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.RectF;
import android.view.MotionEvent;
import android.view.View;

/** A 12 x 8 editor. Each changed cell notifies the Activity for live transmission. */
public final class MatrixGridView extends View {
    private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final boolean[][] pixels = new boolean[MatrixProtocol.ROWS][MatrixProtocol.COLUMNS];
    private OnChangeListener listener;
    private int lastCell=-1;
    private boolean strokeValue;
    private final float inset;
    private final RectF panel = new RectF();
    public interface OnChangeListener { void changed(); }
    public MatrixGridView(Context context) { this(context, null); }
    public MatrixGridView(Context context, AttributeSet attrs) {
        super(context, attrs);
        inset=12*getResources().getDisplayMetrics().density;
    }
    public void setOnChangeListener(OnChangeListener l) { listener = l; }
    public boolean[][] copyPixels() { boolean[][] out = new boolean[8][12]; for (int r=0;r<8;r++) System.arraycopy(pixels[r],0,out[r],0,12); return out; }
    public void setPixels(boolean[][] value) { for (int r=0;r<8;r++) System.arraycopy(value[r],0,pixels[r],0,12); invalidate(); }
    @Override protected void onMeasure(int widthSpec,int heightSpec){
        int width=MeasureSpec.getSize(widthSpec);
        setMeasuredDimension(width,resolveSize(Math.round((width-2*inset)*8f/12f+2*inset),heightSpec));
    }
    @Override protected void onSizeChanged(int width,int height,int oldWidth,int oldHeight){
        float availableWidth=Math.max(0,width-2*inset),availableHeight=Math.max(0,height-2*inset);
        float cell=Math.min(availableWidth/12f,availableHeight/8f);
        float panelWidth=cell*12+2*inset,panelHeight=cell*8+2*inset;
        panel.set((width-panelWidth)/2f,(height-panelHeight)/2f,(width+panelWidth)/2f,(height+panelHeight)/2f);
    }
    @Override protected void onDraw(Canvas canvas) {
        super.onDraw(canvas);
        paint.setStyle(Paint.Style.FILL);paint.setColor(0xff1d302e);
        canvas.drawRoundRect(panel,inset,inset,paint);
        float cw=(panel.width()-2*inset)/12f,ch=(panel.height()-2*inset)/8f;
        for (int r=0;r<8;r++) for (int c=0;c<12;c++) {
            float x=panel.left+inset+(c+.5f)*cw,y=panel.top+inset+(r+.5f)*ch;
            if(pixels[r][c]){paint.setColor(0x28ffc84d);canvas.drawCircle(x,y,Math.min(cw,ch)*.43f,paint);}
            paint.setColor(pixels[r][c] ? 0xffffce61 : 0xff3b504b);
            float radius=Math.min(cw,ch)*.28f;
            canvas.drawRoundRect(x-radius,y-radius,x+radius,y+radius,radius*.55f,radius*.55f,paint);
        }
    }
    @Override public boolean onTouchEvent(MotionEvent event) {
        int action=event.getActionMasked();
        if(action==MotionEvent.ACTION_UP||action==MotionEvent.ACTION_CANCEL){
            getParent().requestDisallowInterceptTouchEvent(false);lastCell=-1;
            if(action==MotionEvent.ACTION_UP)performClick();return true;
        }
        if(action!=MotionEvent.ACTION_DOWN&&action!=MotionEvent.ACTION_MOVE)return true;
        float areaWidth=panel.width()-2*inset,areaHeight=panel.height()-2*inset;
        if(areaWidth<=0||areaHeight<=0)return false;
        if(action==MotionEvent.ACTION_DOWN){
            if(event.getX()<panel.left+inset||event.getX()>=panel.right-inset||event.getY()<panel.top+inset||event.getY()>=panel.bottom-inset)return false;
            getParent().requestDisallowInterceptTouchEvent(true);lastCell=-1;
        }
        int c=(int)Math.floor((event.getX()-panel.left-inset)*12/areaWidth);
        int r=(int)Math.floor((event.getY()-panel.top-inset)*8/areaHeight);
        if (r>=0&&r<8&&c>=0&&c<12&&lastCell!=r*12+c) {
            if(action==MotionEvent.ACTION_DOWN)strokeValue=!pixels[r][c];
            lastCell=r*12+c;
            if(pixels[r][c]!=strokeValue){pixels[r][c]=strokeValue;invalidate();if(listener!=null)listener.changed();}
        }
        return true;
    }
    @Override public boolean performClick(){super.performClick();return true;}
}

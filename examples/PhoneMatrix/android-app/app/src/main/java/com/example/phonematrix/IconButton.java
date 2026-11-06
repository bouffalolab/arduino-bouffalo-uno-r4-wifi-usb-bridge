package com.example.phonematrix;

import android.content.Context;
import android.content.res.TypedArray;
import android.util.AttributeSet;
import android.graphics.Canvas;
import android.os.Build;
import android.widget.Button;
import android.widget.Toast;

/** Icon-only button with an accessible English action name. */
public final class IconButton extends Button {
    private ActionIcon icon;
    public IconButton(Context context) { this(context, null); }
    public IconButton(Context context, AttributeSet attrs) {
        super(context, attrs);
        setText("");
        TypedArray values = context.obtainStyledAttributes(attrs, R.styleable.IconButton);
        String action = values.getString(R.styleable.IconButton_matrixAction);
        values.recycle();
        if (action != null) setAction(action);
    }
    public void setAction(String action) {
        icon = new ActionIcon(getResources(), action);
        String name;
        switch(action){
            case "Heart": name=getResources().getString(R.string.display_heart); break;
            case "Smile": name=getResources().getString(R.string.display_smile); break;
            case "Checker": name=getResources().getString(R.string.display_checker); break;
            case "Clear": name=getResources().getString(R.string.clear_matrix); break;
            case "Stop demo": name=getResources().getString(R.string.stop_loop); break;
            default: name=getResources().getString(R.string.play_loop); break;
        }
        setContentDescription(name);
        if (Build.VERSION.SDK_INT >= 26) setTooltipText(name);
        else setOnLongClickListener(v->{Toast.makeText(getContext(),name,Toast.LENGTH_SHORT).show();return true;});
        invalidate();
    }
    @Override protected void onDraw(Canvas canvas) {
        super.onDraw(canvas);
        if (icon == null) return;
        float density = getResources().getDisplayMetrics().density;
        int width = Math.round(32 * density), height = width;
        int left = (getWidth() - width) / 2, top = (getHeight() - height) / 2;
        icon.setBounds(left, top, left + width, top + height);
        icon.draw(canvas);
    }
}

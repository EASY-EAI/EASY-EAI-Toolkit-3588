//=====================  C++  =====================
#include <string>
//=====================   C   =====================
#include "system.h"
//=====================  PRJ  =====================
#include "display.h"

char **dispBufferMap(Display_t *dispDesc)
{
    static char *pBuffer = NULL;
    static Display_t stDispDesc = {0};

    if((dispDesc->width != stDispDesc.width)||(dispDesc->height != stDispDesc.height)){
        if(pBuffer){
            free(pBuffer);
            pBuffer = NULL;
        }
        memcpy(&stDispDesc, dispDesc, sizeof(Display_t));
    }
    
    if(NULL == pBuffer)
        pBuffer = (char *)malloc(3 * dispDesc->width * dispDesc->height);
    
    return &pBuffer;
}


static Display_t *gDispDesc = NULL;
static gboolean showWidget(GtkWidget *pImage) {
    char **ppBuf = dispBufferMap(gDispDesc);
    const guchar *pBuf = (const guchar *)*ppBuf;
    if(NULL == pBuf){
        return G_SOURCE_CONTINUE;
    }
    
    // 创建一个GdkPixbuf
    GdkPixbuf *pixbuf = gdk_pixbuf_new_from_data(pBuf, GDK_COLORSPACE_RGB, FALSE, 8, gDispDesc->width, gDispDesc->height, 3*gDispDesc->width, NULL, NULL);
    // 将GdkPixbuf设置到图像控件
    gtk_image_set_from_pixbuf(GTK_IMAGE(pImage), pixbuf);
    // 在使用完后释放GdkPixbuf
    g_object_unref(pixbuf);
    
    return G_SOURCE_CONTINUE;
}
static GtkWidget *disp_init(const char *strWinTitle, int32_t width, int32_t height)
{
    gtk_init(NULL, NULL); // 初始化 GTK+ 库
    
    static GtkWidget *pWindow = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    if(pWindow){
        g_signal_connect(pWindow, "destroy", G_CALLBACK(gtk_main_quit), NULL);
        gtk_window_set_title(GTK_WINDOW(pWindow), strWinTitle);
        gtk_window_set_default_size(GTK_WINDOW(pWindow), width, height);

    }else{
        return NULL;
    }

    return pWindow;
}
static int32_t disp_set_loop(GtkWidget *pWindow, GSourceFunc pCamUpdate)
{
    if(NULL == pWindow){
        return -1;
    }
#if 0
    // 创建一个绘图区域
    //GtkWidget *drawing_area = gtk_drawing_area_new();
#else
    // 创建一个Image对象
    GtkWidget *image = gtk_image_new();
#endif
    gtk_container_add(GTK_CONTAINER(pWindow), image/*drawing_area*/);
    // 启动一个循环，定期更新绘图区域显示视频帧
    GSource *gSource = g_timeout_source_new(33); // 33ms，30帧/秒
    g_source_set_callback(gSource, pCamUpdate, image/*drawing_area*/, NULL);
    g_source_attach(gSource, NULL);
    return 0;
}
int display(Display_t *disp)
{
    GtkWidget *pWindow = disp_init(disp->winTitle, disp->width, disp->height);
    if(pWindow){
        gDispDesc = disp;        
        disp_set_loop(pWindow, (GSourceFunc)showWidget);
        
        gtk_widget_show_all(pWindow);
    } 
    // 进入主循环，等待用户操作
    gtk_main();

    return 0;
}


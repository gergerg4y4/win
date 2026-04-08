#include <jni.h>
#include <dlfcn.h>
#include <pthread.h>
#include <unistd.h>
#include <android/log.h>
#include <cstdio>
#include <cstring>
#include <sys/mman.h>

#define TAG "stuffmods"
#define LOG(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)

static void* base = nullptr;
static void* rva(uint64_t o) { return (uint8_t*)base+o; }
template<typename T> static T   rp(void* p,size_t o){return *(T*)((uint8_t*)p+o);}
template<typename T> static void wp(void* p,size_t o,T v){*(T*)((uint8_t*)p+o)=v;}

// ── Mod toggles ───────────────────────────────────────────────────────────────
static bool g_infAmmo   = true;
static bool g_rapidFire = true;
static bool g_maxCurr   = true;
static bool g_menuOpen  = true;
static int  g_selected  = 0;
static int  g_frame     = 0;

struct Item { const char* name; bool* val; };
static Item g_items[] = {
    {"Inf Ammo",    &g_infAmmo  },
    {"Rapid Fire",  &g_rapidFire},
    {"Max Currency",&g_maxCurr  },
};
static const int N = 3;

// ── Hook infra ────────────────────────────────────────────────────────────────
typedef int(*DobbyHook_t)(void*,void*,void**);
static DobbyHook_t _dobby = nullptr;

static bool hook(void* tgt, void* rep, void** orig) {
    if (!_dobby) {
        _dobby=(DobbyHook_t)dlsym(RTLD_DEFAULT,"DobbyHook");
        if (!_dobby) _dobby=(DobbyHook_t)dlsym(RTLD_DEFAULT,"A64HookFunction");
    }
    if (_dobby) { int r=_dobby(tgt,rep,orig); LOG("Dobby %p r=%d",tgt,r); return r==0; }
    // Manual trampoline
    uintptr_t pg=(uintptr_t)tgt&~0xFFFull;
    mprotect((void*)pg,0x2000,PROT_READ|PROT_WRITE|PROT_EXEC);
    if (orig) {
        uint8_t* tr=(uint8_t*)mmap(nullptr,32,PROT_READ|PROT_WRITE|PROT_EXEC,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
        if (tr!=MAP_FAILED) {
            memcpy(tr,tgt,16);
            uint32_t j[4]={0x58000051,0xD61F0220,0,0};
            uintptr_t c=(uintptr_t)tgt+16; memcpy(&j[2],&c,8);
            memcpy(tr+16,j,16); __builtin___clear_cache((char*)tr,(char*)tr+32);
            *orig=tr;
        }
    }
    uint32_t p[4]={0x58000051,0xD61F0220,0,0};
    memcpy(&p[2],&rep,8); memcpy(tgt,p,16);
    __builtin___clear_cache((char*)tgt,(char*)tgt+16);
    return true;
}

// ── IL2CPP string helper ──────────────────────────────────────────────────────
typedef void*(*StrNew_t)(const char*);
static StrNew_t _strNew = nullptr;

// ── GUI.Label(Rect xywh, string) ─────────────────────────────────────────────
// Rect is 4 floats: x,y,w,h passed as struct by value on ARM64
// ARM64 HFA rule: struct of 4 floats passed in s0-s3
typedef void(*GUILabel_t)(float,float,float,float, void*);
static GUILabel_t _guiLabel = nullptr;

// GUI.color = Color setter RVA: 0x46D8908
typedef void(*GUISetColor_t)(float,float,float,float);
static GUISetColor_t _guiSetColor = nullptr;

// Screen.width/height getters
typedef int(*ScreenDim_t)();
static ScreenDim_t _screenW = nullptr;
static ScreenDim_t _screenH = nullptr;

static void drawMenu() {
    if (!_guiLabel || !_strNew) { LOG("drawMenu: missing guiLabel=%p strNew=%p", (void*)_guiLabel, (void*)_strNew); return; }
    static int dc = 0; dc++;
    if (dc % 300 == 1) LOG("drawMenu firing dc=%d", dc);
    int sw = _screenW ? _screenW() : 1832;
    int sh = _screenH ? _screenH() : 1920;

    float lh  = 28.0f;   // line height px
    float w   = 220.0f;
    float x   = sw - w - 10.0f;  // top-right
    float y   = 10.0f;

    // Title
    if (_guiSetColor) _guiSetColor(1.0f, 0.55f, 0.0f, 1.0f); // orange
    _guiLabel(x, y, w, lh, _strNew("=== StuffMods ==="));
    y += lh + 2;

    // Items
    for (int i = 0; i < N; i++) {
        bool sel = (i == g_selected);
        char buf[128];
        snprintf(buf, sizeof(buf), "%s%s [%s]",
            sel ? "-> " : "   ",
            g_items[i].name,
            *g_items[i].val ? "ON" : "OFF");
        if (_guiSetColor) {
            if (sel)              _guiSetColor(1.f,1.f,1.f,1.f);     // white = selected
            else if (*g_items[i].val) _guiSetColor(0.f,1.f,0.f,1.f); // green = enabled
            else                  _guiSetColor(1.f,0.55f,0.f,1.f);   // orange = disabled
        }
        _guiLabel(x, y, w, lh, _strNew(buf));
        y += lh;
    }

    // Hint
    if (_guiSetColor) _guiSetColor(0.5f,0.5f,0.5f,1.f);
    _guiLabel(x, y+4, w, lh, _strNew("X=next  A=toggle"));

    // Reset color
    if (_guiSetColor) _guiSetColor(1.f,1.f,1.f,1.f);
}

// ── AmplifyOcclusionEffect.OnGUI hook (RVA 0x20AB808, 3924 bytes) ────────────
typedef void(*OnGUI_t)(void*);
static OnGUI_t _onGUIorig = nullptr;
static int g_onGUIcount = 0;
static void onGUIhook(void* self) {
    if (_onGUIorig) _onGUIorig(self);
    g_onGUIcount++;
    if (g_onGUIcount % 300 == 1) LOG("onGUI firing count=%d", g_onGUIcount);
    if (g_menuOpen) drawMenu();
}

// ── Input reading ─────────────────────────────────────────────────────────────
// InputDevices.GetDeviceAtXRNode(XRNode node) RVA: 0x453EC08
// InputDevice.TryGetFeatureValue(InputFeatureUsage<bool>, out bool) RVA: 0x453F110
// CommonUsages.secondaryButton getter RVA: 0x453E4A0
// CommonUsages.primaryButton getter RVA: 0x453E490
// CommonUsages.primaryButton2D getter (thumbstick) RVA: 0x453E5B0

typedef void*(*GetDevice_t)(int);
typedef bool(*TryGetBool_t)(void*,void*,void*);
typedef void(*GetUsage_t)(void*);

static GetDevice_t _getDevice = nullptr;
static TryGetBool_t _tryBool  = nullptr;
static uint8_t _usageY[32]    = {}; // secondaryButton (Y)
static uint8_t _usageX[32]    = {}; // primaryButton (X)
static uint8_t _usageA[32]    = {}; // primaryButton right (A)

static bool readBtn(int node, void* usage) {
    if (!_getDevice||!_tryBool||!usage) return false;
    void* dev = _getDevice(node);
    if (!dev) return false;
    uint8_t out=0;
    _tryBool(dev,usage,&out);
    return out!=0;
}

// ── ShamcerManager.Update (RVA 0x20CA130) ────────────────────────────────────
typedef void(*ShamUpd_t)(void*);
static ShamUpd_t _shamOrig=nullptr;
static void shamHook(void* s){
    if(_shamOrig)_shamOrig(s); if(!s)return;
    if(g_infAmmo){int32_t m=rp<int32_t>(s,0x88);if(m>0&&m<9999){wp<int32_t>(s,0x98,m);wp<uint8_t>(s,0x9C,0);}}
    if(g_rapidFire){wp<float>(s,0x8C,0.f);wp<uint8_t>(s,0x9C,0);}
}

// ── ShotPungManager.Update (RVA 0x20E0304) ───────────────────────────────────
typedef void(*ShotUpd_t)(void*);
static ShotUpd_t _shotOrig=nullptr;
static void shotHook(void* s){
    if(_shotOrig)_shotOrig(s); if(!s)return;
    if(g_infAmmo||g_rapidFire){wp<uint8_t>(s,0x40,1);wp<uint8_t>(s,0x98,0);wp<int32_t>(s,0x9C,0);}
}

// ── CGM.LateUpdate (RVA 0x20AFB28) ───────────────────────────────────────────
typedef void(*CGMLate_t)(void*);
static CGMLate_t _cgmOrig=nullptr;

static bool _lastY=false, _lastX=false, _lastA=false;

static void cgmHook(void* self){
    if(_cgmOrig)_cgmOrig(self);
    g_frame++;

    if(g_maxCurr&&g_frame%300==0){
        wp<int32_t>(self,0xD0,999999);
        wp<int32_t>(self,0xE4,999999);
    }

    // Input
    bool Y = readBtn(4, _usageY); // left hand Y = menu toggle
    bool X = readBtn(4, _usageX); // left hand X = next item
    bool A = readBtn(5, _usageA); // right hand A = select/toggle

    // Menu always open - Y unused, X=next, A=select
    if(X&&!_lastX){ g_selected=(g_selected+1)%N; }
    if(A&&!_lastA){ *g_items[g_selected].val=!*g_items[g_selected].val;
        LOG("%s = %d",g_items[g_selected].name,*g_items[g_selected].val); }
    _lastY=Y; _lastX=X; _lastA=A;
}

// ── Setup ─────────────────────────────────────────────────────────────────────
static void* setup(void*){
    sleep(20); // wait well past loading screen

    char path[512]={};
    {FILE* m=fopen("/proc/self/maps","r"); char l[512];
    while(m&&fgets(l,sizeof(l),m)){
        if(strstr(l,"libil2cpp.so")&&strstr(l,"r-xp")){
            uint64_t b=0;sscanf(l,"%llx",(unsigned long long*)&b);base=(void*)b;
            char* p=strrchr(l,' ');if(!p)p=strrchr(l,'\t');
            if(p){strncpy(path,p+1,sizeof(path)-1);path[strcspn(path,"\n")]=0;}
            LOG("base=%p",base);break;
        }
    }if(m)fclose(m);}
    if(!base){LOG("no libil2cpp");return nullptr;}

    void* h=dlopen(path[0]?path:"libil2cpp.so",RTLD_NOLOAD|RTLD_NOW|RTLD_GLOBAL);
    void* src=h?h:RTLD_DEFAULT;
    _strNew=(StrNew_t)dlsym(src,"il2cpp_string_new");
    LOG("strNew=%p",(void*)_strNew);

    // GUI RVAs
    _guiLabel    =(GUILabel_t)   rva(0x46D90BC);
    _guiSetColor =(GUISetColor_t)rva(0x46D8908);

    // Screen RVAs (get_width, get_height)
    _screenW=(ScreenDim_t)rva(0x46B9970);
    _screenH=(ScreenDim_t)rva(0x46B9988);

    // Skip input for now - menu is always open, mods always on
    _getDevice = nullptr;
    _tryBool   = nullptr;

    // Hook all methods - stagger them to avoid init race
    sleep(2);
    LOG("hooking CGM LateUpdate...");
    hook(rva(0x20AFB28),(void*)cgmHook,  (void**)&_cgmOrig);  // CGM first
    sleep(1);
    LOG("hooking OnGUI...");
    hook(rva(0x20AB808),(void*)onGUIhook,(void**)&_onGUIorig); // GUI
    sleep(1);
    hook(rva(0x20CA130),(void*)shamHook, (void**)&_shamOrig);  // weapons
    hook(rva(0x20E0304),(void*)shotHook, (void**)&_shotOrig);

    LOG("stuffmods ready");
    return nullptr;
}

extern "C" JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm,void*){
    (void)vm;
    LOG("stuffmods loaded!");
    pthread_t t;
    pthread_create(&t,nullptr,setup,nullptr);
    pthread_detach(t);
    return JNI_VERSION_1_6;
}

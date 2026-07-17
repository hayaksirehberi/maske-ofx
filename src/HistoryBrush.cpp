// History Brush — OFX plugin for DaVinci Resolve (Color page)
//
// Paints a per-frame matte in the viewer and outputs it in the alpha channel.
// Enable "Use OFX Alpha" on the node, route the node's key output into the
// dirt-removal node's key input (inverted) to suppress the effect in painted
// areas — a Color-page equivalent of a history brush.

#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <string>
#include <vector>
#include <map>
#include <mutex>
#include <algorithm>
#include <thread>
#include <functional>

#include "ofxCore.h"
#include "ofxProperty.h"
#include "ofxImageEffect.h"
#include "ofxParam.h"
#include "ofxInteract.h"
#include "ofxKeySyms.h"
#include "ofxDrawSuite.h"
#include "ofxPixels.h"
#include "ofxGPURender.h"

#include "GpuBackend.h"

// ---------------------------------------------------------------------------
// Globals: host + suites
// ---------------------------------------------------------------------------

static OfxHost*               gHost         = nullptr;
static OfxPropertySuiteV1*    gPropSuite    = nullptr;
static OfxImageEffectSuiteV1* gEffectSuite  = nullptr;
static OfxParameterSuiteV1*   gParamSuite   = nullptr;
static OfxInteractSuiteV1*    gInteractSuite= nullptr;
static OfxDrawSuiteV1*        gDrawSuite    = nullptr;

#define PLUGIN_ID    "com.mustafaekinci.Maske"
#define PLUGIN_LABEL "Maske"
#define PLUGIN_GROUP "Mustafa Ekinci"

// Param names
#define P_BRUSH_SIZE "brushSize"
#define P_SOFTNESS   "brushSoftness"
#define P_MODE       "brushMode"      // 0 = paint, 1 = erase
#define P_SHOW_MATTE "showMatte"
#define P_INVERT     "invertMatte"
#define P_STROKES    "strokeData"     // serialized stamps, secret
#define P_NUDGE      "renderNudge"    // secret, non-persistent; bumped per motion to trigger re-render
#define P_CLEAR_FRAME "clearFrame"
#define P_CLEAR_ALL   "clearAll"

// ---------------------------------------------------------------------------
// Stroke model
// ---------------------------------------------------------------------------

struct Stamp {
    double x, y;     // canonical coords
    double size;     // canonical diameter
    double soft;     // 0..1
    int    erase;    // 0 paint, 1 erase
};

struct EffectInstance {
    std::mutex mtx;
    std::map<int, std::vector<Stamp>> frames;   // frame -> stamps in order

    // interact state (single viewer assumption is fine for Resolve)
    double cursorX = 0, cursorY = 0;
    bool   cursorValid = false;
    bool   penDown = false;
    double lastStampX = 0, lastStampY = 0;
    int    nudgeCounter = 0;

    // Opt/Alt-drag brush adjust: hold Alt and drag — horizontal changes size,
    // vertical changes softness. altHeld is tracked from key events; a stroke's
    // mode is locked in at pen-down so a mid-stroke key glitch can't flip it.
    bool   altHeld = false;
    bool   adjusting = false;
    double adjStartX = 0, adjStartY = 0;      // pen-down anchor (canonical)
    double adjStartSize = 0, adjStartSoft = 0;

    std::string serialize() {
        std::string out;
        char buf[128];
        for (auto& kv : frames) {
            for (auto& s : kv.second) {
                snprintf(buf, sizeof(buf), "%d %.3f %.3f %.3f %.4f %d;",
                         kv.first, s.x, s.y, s.size, s.soft, s.erase);
                out += buf;
            }
        }
        return out;
    }

    void parse(const char* str) {
        frames.clear();
        if (!str) return;
        const char* p = str;
        while (*p) {
            int frame = 0, erase = 0;
            double x, y, size, soft;
            int n = 0;
            if (sscanf(p, "%d %lf %lf %lf %lf %d;%n", &frame, &x, &y, &size, &soft, &erase, &n) >= 6 && n > 0) {
                frames[frame].push_back({x, y, size, soft, erase});
                p += n;
            } else {
                const char* semi = strchr(p, ';');
                if (!semi) break;
                p = semi + 1;
            }
        }
    }
};

// ---------------------------------------------------------------------------
// Small property/param helpers
// ---------------------------------------------------------------------------

static OfxPropertySetHandle effectProps(OfxImageEffectHandle effect) {
    OfxPropertySetHandle props = nullptr;
    gEffectSuite->getPropertySet(effect, &props);
    return props;
}

static EffectInstance* instanceData(OfxImageEffectHandle effect) {
    void* ptr = nullptr;
    gPropSuite->propGetPointer(effectProps(effect), kOfxPropInstanceData, 0, &ptr);
    return (EffectInstance*)ptr;
}

static OfxParamHandle getParam(OfxImageEffectHandle effect, const char* name) {
    OfxParamSetHandle paramSet = nullptr;
    gEffectSuite->getParamSet(effect, &paramSet);
    OfxParamHandle param = nullptr;
    gParamSuite->paramGetHandle(paramSet, name, &param, nullptr);
    return param;
}

static double getDouble(OfxImageEffectHandle effect, const char* name) {
    double v = 0;
    gParamSuite->paramGetValue(getParam(effect, name), &v);
    return v;
}

static int getChoice(OfxImageEffectHandle effect, const char* name) {
    int v = 0;
    gParamSuite->paramGetValue(getParam(effect, name), &v);
    return v;
}

static int getBool(OfxImageEffectHandle effect, const char* name) {
    int v = 0;
    gParamSuite->paramGetValue(getParam(effect, name), &v);
    return v;
}

static void setStrokesParam(OfxImageEffectHandle effect, const std::string& data) {
    gParamSuite->paramSetValue(getParam(effect, P_STROKES), data.c_str());
}

static void reparseFromParam(OfxImageEffectHandle effect, EffectInstance* inst) {
    char* str = nullptr;
    gParamSuite->paramGetValue(getParam(effect, P_STROKES), &str);
    std::lock_guard<std::mutex> lock(inst->mtx);
    inst->parse(str);
}

// ---------------------------------------------------------------------------
// Interact (viewer brush)
// ---------------------------------------------------------------------------

static OfxImageEffectHandle interactEffect(OfxPropertySetHandle inArgs) {
    void* ptr = nullptr;
    gPropSuite->propGetPointer(inArgs, kOfxPropEffectInstance, 0, &ptr);
    return (OfxImageEffectHandle)ptr;
}

static void addStamp(OfxImageEffectHandle effect, EffectInstance* inst,
                     double x, double y, double time, bool force) {
    double size = getDouble(effect, P_BRUSH_SIZE);
    double soft = getDouble(effect, P_SOFTNESS);
    int erase = getChoice(effect, P_MODE);
    int frame = (int)floor(time + 0.5);

    {
        std::lock_guard<std::mutex> lock(inst->mtx);
        if (!force) {
            double dx = x - inst->lastStampX, dy = y - inst->lastStampY;
            if (sqrt(dx * dx + dy * dy) < size * 0.15) return; // stamp spacing
        }
        inst->frames[frame].push_back({x, y, size, soft, erase});
        inst->lastStampX = x;
        inst->lastStampY = y;
    }
    // Full stroke data is serialized once on pen-up; during the stroke a cheap
    // counter bump is enough to make the host re-render from the in-memory model.
    gParamSuite->paramSetValue(getParam(effect, P_NUDGE), ++inst->nudgeCounter);
}

// Filled disc via a polygon fan. Ellipse in DrawSuite is an outline only, and
// thin outlines break up / vanish when the viewer is zoomed out; a filled
// polygon stays solid at any zoom.
static void fillDisc(OfxDrawContextHandle ctx, double cx, double cy, double radius) {
    const int kSegs = 64;
    const double kPI = 3.14159265358979323846;
    OfxPointD pts[kSegs];
    for (int i = 0; i < kSegs; ++i) {
        double a = 2.0 * kPI * i / kSegs;
        pts[i] = {cx + radius * cos(a), cy + radius * sin(a)};
    }
    gDrawSuite->draw(ctx, kOfxDrawPrimitivePolygon, pts, kSegs);
}

static OfxStatus interactMain(const char* action, const void* handle,
                              OfxPropertySetHandle inArgs, OfxPropertySetHandle /*outArgs*/) {
    OfxInteractHandle interact = (OfxInteractHandle)handle;

    if (strcmp(action, kOfxActionDescribe) == 0 ||
        strcmp(action, kOfxActionCreateInstance) == 0 ||
        strcmp(action, kOfxActionDestroyInstance) == 0) {
        return kOfxStatOK;
    }

    if (strcmp(action, kOfxInteractActionDraw) == 0) {
        if (!gDrawSuite) return kOfxStatReplyDefault;
        void* ctxPtr = nullptr;
        gPropSuite->propGetPointer(inArgs, kOfxInteractPropDrawContext, 0, &ctxPtr);
        OfxDrawContextHandle ctx = (OfxDrawContextHandle)ctxPtr;
        if (!ctx) return kOfxStatReplyDefault;

        OfxImageEffectHandle effect = interactEffect(inArgs);
        EffectInstance* inst = effect ? instanceData(effect) : nullptr;
        if (!inst || !inst->cursorValid) return kOfxStatOK;

        double size = getDouble(effect, P_BRUSH_SIZE);
        double soft = getDouble(effect, P_SOFTNESS);
        double r = size * 0.5;
        int erase = getChoice(effect, P_MODE);
        double cx = inst->cursorX, cy = inst->cursorY;

        // Photoshop-style translucent brush highlight, drawn as filled discs so
        // it stays solid at every zoom level (thin outlines broke up / vanished
        // when zoomed out). Paint is red, erase is blue. Softness reads as a
        // denser hard core sitting inside a fainter full-radius halo.
        OfxRGBAColourF tint = erase ? OfxRGBAColourF{0.35f, 0.55f, 1.0f, 1.0f}
                                    : OfxRGBAColourF{1.0f, 0.22f, 0.22f, 1.0f};
        // Yellow while Opt is held / adjusting — also confirms at a glance that
        // Resolve is delivering the modifier to the plugin.
        if (inst->altHeld || inst->adjusting)
            tint = OfxRGBAColourF{1.0f, 0.80f, 0.15f, 1.0f};

        OfxRGBAColourF halo = tint; halo.a = 0.18f;
        gDrawSuite->setColour(ctx, &halo);
        fillDisc(ctx, cx, cy, r);

        OfxRGBAColourF core = tint; core.a = 0.30f;  // hard core, radius * (1 - softness)
        gDrawSuite->setColour(ctx, &core);
        fillDisc(ctx, cx, cy, r * (1.0 - soft));

        // Crisp edge ring on top so the exact brush boundary is readable.
        OfxRGBAColourF edge = tint; edge.a = 0.95f;
        gDrawSuite->setColour(ctx, &edge);
        gDrawSuite->setLineWidth(ctx, 2.0f);
        OfxPointD outer[2] = {{cx - r, cy - r}, {cx + r, cy + r}};
        gDrawSuite->draw(ctx, kOfxDrawPrimitiveEllipse, outer, 2);

        // Numeric readout pinned a constant screen distance from the cursor so
        // it stays legible regardless of zoom (offsets are in canonical units,
        // scaled by the viewer's pixel scale).
        double pscale[2] = {1.0, 1.0};
        gPropSuite->propGetDoubleN(inArgs, kOfxInteractPropPixelScale, 2, pscale);
        double mx = (pscale[0] > 0 ? 14.0 / pscale[0] : 14.0);
        double my = (pscale[1] > 0 ? 14.0 / pscale[1] : 14.0);
        char label[64];
        snprintf(label, sizeof(label), "%d px   soft %d%%",
                 (int)lround(size), (int)lround(soft * 100.0));
        OfxPointD tp = {cx + mx, cy + my};
        gDrawSuite->drawText(ctx, label, &tp,
                             kOfxDrawTextAlignmentLeft | kOfxDrawTextAlignmentBottom);
        return kOfxStatOK;
    }

    // Track the Alt/Option modifier for Opt-drag brush adjust. Keyboard shortcuts
    // proved unusable (Resolve grabs keys like the arrows for clip navigation), so
    // we only watch the modifier and do the actual adjust with the mouse in the
    // pen handler below. Bare Alt isn't bound to a Resolve action, so it has a
    // better chance of reaching the interact than a bound key does — but if it
    // doesn't arrive, altHeld simply stays false and dragging paints as usual.
    if (strcmp(action, kOfxInteractActionKeyDown) == 0 ||
        strcmp(action, kOfxInteractActionKeyRepeat) == 0 ||
        strcmp(action, kOfxInteractActionKeyUp) == 0) {
        OfxImageEffectHandle effect = interactEffect(inArgs);
        EffectInstance* inst = effect ? instanceData(effect) : nullptr;
        if (!inst) return kOfxStatReplyDefault;
        int key = 0;
        gPropSuite->propGetInt(inArgs, kOfxPropKeySym, 0, &key);
        if (key == kOfxKey_Alt_L || key == kOfxKey_Alt_R) {
            inst->altHeld = (strcmp(action, kOfxInteractActionKeyUp) != 0);
            gInteractSuite->interactRedraw(interact);  // recolour highlight as feedback
            return kOfxStatOK;
        }
        return kOfxStatReplyDefault;
    }

    bool isPenDown   = strcmp(action, kOfxInteractActionPenDown) == 0;
    bool isPenMotion = strcmp(action, kOfxInteractActionPenMotion) == 0;
    bool isPenUp     = strcmp(action, kOfxInteractActionPenUp) == 0;

    if (isPenDown || isPenMotion || isPenUp) {
        OfxImageEffectHandle effect = interactEffect(inArgs);
        if (!effect) return kOfxStatReplyDefault;
        EffectInstance* inst = instanceData(effect);
        if (!inst) return kOfxStatReplyDefault;

        double pos[2] = {0, 0};
        gPropSuite->propGetDoubleN(inArgs, kOfxInteractPropPenPosition, 2, pos);
        double time = 0;
        gPropSuite->propGetDouble(inArgs, kOfxPropTime, 0, &time);

        inst->cursorX = pos[0];
        inst->cursorY = pos[1];
        inst->cursorValid = true;

        if (isPenDown) {
            if (inst->altHeld) {
                // Opt-drag: adjust the brush instead of painting. Anchor here.
                inst->adjusting = true;
                inst->adjStartX = pos[0];
                inst->adjStartY = pos[1];
                inst->adjStartSize = getDouble(effect, P_BRUSH_SIZE);
                inst->adjStartSoft = getDouble(effect, P_SOFTNESS);
            } else {
                inst->penDown = true;
                OfxParamSetHandle paramSet = nullptr;
                gEffectSuite->getParamSet(effect, &paramSet);
                gParamSuite->paramEditBegin(paramSet, "History Brush stroke");
                addStamp(effect, inst, pos[0], pos[1], time, /*force=*/true);
            }
        } else if (isPenMotion) {
            if (inst->adjusting) {
                // Horizontal drag sizes the brush (its edge tracks the cursor);
                // vertical drag (up = softer) sets softness. Keep the highlight
                // pinned at the anchor so it grows in place, Photoshop-style.
                double dx = pos[0] - inst->adjStartX;
                double dy = pos[1] - inst->adjStartY;
                double newSize = std::min(2000.0, std::max(2.0,  inst->adjStartSize + 2.0 * dx));
                double newSoft = std::min(0.99,   std::max(0.0,  inst->adjStartSoft + dy / 400.0));
                gParamSuite->paramSetValue(getParam(effect, P_BRUSH_SIZE), newSize);
                gParamSuite->paramSetValue(getParam(effect, P_SOFTNESS), newSoft);
                inst->cursorX = inst->adjStartX;
                inst->cursorY = inst->adjStartY;
            } else if (inst->penDown) {
                addStamp(effect, inst, pos[0], pos[1], time, /*force=*/false);
            }
        } else if (isPenUp) {
            if (inst->adjusting) {
                inst->adjusting = false;
            } else if (inst->penDown) {
                inst->penDown = false;
                std::string data;
                {
                    std::lock_guard<std::mutex> lock(inst->mtx);
                    data = inst->serialize();
                }
                setStrokesParam(effect, data);
                OfxParamSetHandle paramSet = nullptr;
                gEffectSuite->getParamSet(effect, &paramSet);
                gParamSuite->paramEditEnd(paramSet);
            }
        }
        gInteractSuite->interactRedraw(interact);
        // Trap pen events while painting or adjusting so Resolve doesn't also
        // pan/select; let plain hover motion pass through.
        bool active = inst->penDown || inst->adjusting;
        return (isPenMotion && !active) ? kOfxStatReplyDefault : kOfxStatOK;
    }

    return kOfxStatReplyDefault;
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------

struct ImageDesc {
    OfxPropertySetHandle handle = nullptr;
    void* data = nullptr;
    OfxRectI bounds = {};
    int rowBytes = 0;
    int depthBytes = 4;      // 1, 2, or 4
    int nComp = 4;
    double par = 1.0;
    bool ok = false;
};

static ImageDesc fetchImage(OfxImageClipHandle clip, OfxTime time) {
    ImageDesc d;
    if (gEffectSuite->clipGetImage(clip, time, nullptr, &d.handle) != kOfxStatOK || !d.handle)
        return d;
    gPropSuite->propGetPointer(d.handle, kOfxImagePropData, 0, &d.data);
    gPropSuite->propGetIntN(d.handle, kOfxImagePropBounds, 4, &d.bounds.x1);
    gPropSuite->propGetInt(d.handle, kOfxImagePropRowBytes, 0, &d.rowBytes);
    gPropSuite->propGetDouble(d.handle, kOfxImagePropPixelAspectRatio, 0, &d.par);
    if (d.par <= 0) d.par = 1.0;

    char* depth = nullptr;
    gPropSuite->propGetString(d.handle, kOfxImageEffectPropPixelDepth, 0, &depth);
    if (depth && strcmp(depth, kOfxBitDepthByte) == 0) d.depthBytes = 1;
    else if (depth && strcmp(depth, kOfxBitDepthShort) == 0) d.depthBytes = 2;
    else d.depthBytes = 4;

    char* comp = nullptr;
    gPropSuite->propGetString(d.handle, kOfxImageEffectPropComponents, 0, &comp);
    d.nComp = (comp && strcmp(comp, kOfxImageComponentAlpha) == 0) ? 1 : 4;

    d.ok = (d.data != nullptr);
    return d;
}

static inline void* pixelAddress(const ImageDesc& img, int x, int y) {
    if (x < img.bounds.x1 || x >= img.bounds.x2 || y < img.bounds.y1 || y >= img.bounds.y2)
        return nullptr;
    char* row = (char*)img.data + (size_t)(y - img.bounds.y1) * img.rowBytes;
    return row + (size_t)(x - img.bounds.x1) * img.nComp * img.depthBytes;
}

static void parallelRows(int y1, int y2, const std::function<void(int, int)>& fn) {
    unsigned n = std::thread::hardware_concurrency();
    if (n == 0) n = 4;
    n = std::min(n, 16u);
    int total = y2 - y1;
    if (total <= 64 || n <= 1) { fn(y1, y2); return; }
    int chunk = (total + (int)n - 1) / (int)n;
    std::vector<std::thread> threads;
    for (unsigned i = 0; i < n; ++i) {
        int a = y1 + (int)i * chunk;
        int b = std::min(y2, a + chunk);
        if (a >= b) break;
        threads.emplace_back(fn, a, b);
    }
    for (auto& t : threads) t.join();
}

template <typename PIX, int MAX>
static void writeOutput(const ImageDesc& src, const ImageDesc& dst, const OfxRectI& win,
                        const std::vector<float>& matte, bool showMatte) {
    int w = win.x2 - win.x1;
    // Row-level memcpy is valid when the source row fully covers the window.
    bool srcRowCopy = src.ok && src.nComp == dst.nComp &&
                      src.bounds.x1 <= win.x1 && src.bounds.x2 >= win.x2;

    parallelRows(win.y1, win.y2, [&](int rowStart, int rowEnd) {
        for (int y = rowStart; y < rowEnd; ++y) {
            PIX* out = (PIX*)pixelAddress(dst, win.x1, y);
            if (!out) continue;
            const float* m = &matte[(size_t)(y - win.y1) * w];

            if (showMatte) {
                PIX* p = out;
                for (int x = 0; x < w; ++x, p += dst.nComp) {
                    PIX a = (PIX)(m[x] * MAX);
                    p[0] = p[1] = p[2] = a;
                    if (dst.nComp == 4) p[3] = a;
                }
                continue;
            }

            bool rowInSrc = srcRowCopy && y >= src.bounds.y1 && y < src.bounds.y2;
            if (rowInSrc) {
                memcpy(out, pixelAddress(src, win.x1, y), (size_t)w * dst.nComp * sizeof(PIX));
            } else {
                PIX* p = out;
                for (int x = win.x1; x < win.x2; ++x, p += dst.nComp) {
                    const PIX* in = src.ok ? (const PIX*)pixelAddress(src, x, y) : nullptr;
                    if (in) { p[0] = in[0]; p[1] = in[1]; p[2] = in[2]; }
                    else    { p[0] = p[1] = p[2] = 0; }
                }
            }
            if (dst.nComp == 4) {
                PIX* p = out + 3;
                for (int x = 0; x < w; ++x, p += 4) *p = (PIX)(m[x] * MAX);
            }
        }
    });
}

static OfxStatus render(OfxImageEffectHandle effect, OfxPropertySetHandle inArgs) {
    OfxTime time = 0;
    gPropSuite->propGetDouble(inArgs, kOfxPropTime, 0, &time);
    OfxRectI win = {};
    gPropSuite->propGetIntN(inArgs, kOfxImageEffectPropRenderWindow, 4, &win.x1);
    double rs[2] = {1, 1};
    gPropSuite->propGetDoubleN(inArgs, kOfxImageEffectPropRenderScale, 2, rs);

    OfxImageClipHandle srcClip = nullptr, dstClip = nullptr;
    gEffectSuite->clipGetHandle(effect, kOfxImageEffectSimpleSourceClipName, &srcClip, nullptr);
    gEffectSuite->clipGetHandle(effect, kOfxImageEffectOutputClipName, &dstClip, nullptr);

    ImageDesc dst = fetchImage(dstClip, time);
    if (!dst.ok) return kOfxStatFailed;
    ImageDesc src = fetchImage(srcClip, time);

    EffectInstance* inst = instanceData(effect);
    int frame = (int)floor(time + 0.5);
    double par = dst.par;

    std::vector<Stamp> stamps;
    if (inst) {
        std::lock_guard<std::mutex> lock(inst->mtx);
        auto it = inst->frames.find(frame);
        if (it != inst->frames.end()) stamps = it->second;
    }

#if defined(HB_HAS_CUDA) || defined(HB_HAS_METAL)
    int cudaOn = 0, metalOn = 0;
    gPropSuite->propGetInt(inArgs, kOfxImageEffectPropCudaEnabled, 0, &cudaOn);
    gPropSuite->propGetInt(inArgs, kOfxImageEffectPropMetalEnabled, 0, &metalOn);
    if (cudaOn || metalOn) {
        // GPU path: image pointers are device buffers, always float RGBA.
        if (dst.depthBytes != 4 || dst.nComp != 4) {
            if (src.handle) gEffectSuite->clipReleaseImage(src.handle);
            gEffectSuite->clipReleaseImage(dst.handle);
            return kOfxStatFailed;
        }
        std::vector<GpuStamp> gpuStamps;
        gpuStamps.reserve(stamps.size());
        for (const Stamp& s : stamps) {
            GpuStamp g;
            g.cx = (float)(s.x * rs[0] / par);
            g.cy = (float)(s.y * rs[1]);
            g.rx = (float)std::max(0.5, s.size * 0.5 * rs[0] / par);
            g.ry = (float)std::max(0.5, s.size * 0.5 * rs[1]);
            g.inner = (float)(1.0 - std::min(std::max(s.soft, 0.0), 0.999));
            g.erase = s.erase;
            gpuStamps.push_back(g);
        }

        GpuRenderArgs ga = {};
        ga.src = src.ok ? src.data : nullptr;
        ga.srcRowElems = src.ok ? src.rowBytes / 4 : 0;
        ga.srcX1 = src.bounds.x1; ga.srcY1 = src.bounds.y1;
        ga.srcX2 = src.bounds.x2; ga.srcY2 = src.bounds.y2;
        ga.dst = dst.data;
        ga.dstRowElems = dst.rowBytes / 4;
        ga.dstX1 = dst.bounds.x1; ga.dstY1 = dst.bounds.y1;
        ga.winX1 = win.x1; ga.winY1 = win.y1;
        ga.winX2 = win.x2; ga.winY2 = win.y2;
        ga.stamps = gpuStamps.data();
        ga.nStamps = (int)gpuStamps.size();
        ga.invert = getBool(effect, P_INVERT);
        ga.showMatte = getBool(effect, P_SHOW_MATTE);

        bool ok = false;
#ifdef HB_HAS_CUDA
        if (cudaOn) {
            void* stream = nullptr;
            gPropSuite->propGetPointer(inArgs, kOfxImageEffectPropCudaStream, 0, &stream);
            ok = HistoryBrushCudaRender(stream, &ga);
        }
#endif
#ifdef HB_HAS_METAL
        if (metalOn) {
            void* queue = nullptr;
            gPropSuite->propGetPointer(inArgs, kOfxImageEffectPropMetalCommandQueue, 0, &queue);
            ok = HistoryBrushMetalRender(queue, &ga);
        }
#endif
        if (src.handle) gEffectSuite->clipReleaseImage(src.handle);
        gEffectSuite->clipReleaseImage(dst.handle);
        return ok ? kOfxStatOK : kOfxStatFailed;
    }
#endif

    // CPU path: rasterize stamps for this frame into a matte buffer.
    int w = win.x2 - win.x1, h = win.y2 - win.y1;
    std::vector<float> matte((size_t)w * h, 0.0f);

    if (!stamps.empty()) {
        // Rows are independent; each chunk walks the stamps in stroke order,
        // preserving paint/erase sequencing per pixel.
        parallelRows(win.y1, win.y2, [&](int rowStart, int rowEnd) {
            for (const Stamp& s : stamps) {
                double cx = s.x * rs[0] / par;
                double cy = s.y * rs[1];
                double rx = s.size * 0.5 * rs[0] / par;
                double ry = s.size * 0.5 * rs[1];
                if (rx < 0.5) rx = 0.5;
                if (ry < 0.5) ry = 0.5;
                double inner = 1.0 - std::min(std::max(s.soft, 0.0), 0.999);

                int x1 = std::max(win.x1, (int)floor(cx - rx));
                int x2 = std::min(win.x2, (int)ceil(cx + rx) + 1);
                int y1 = std::max(rowStart, (int)floor(cy - ry));
                int y2 = std::min(rowEnd, (int)ceil(cy + ry) + 1);

                for (int y = y1; y < y2; ++y) {
                    for (int x = x1; x < x2; ++x) {
                        double nx = (x + 0.5 - cx) / rx;
                        double ny = (y + 0.5 - cy) / ry;
                        double d = sqrt(nx * nx + ny * ny);
                        if (d >= 1.0) continue;
                        double c;
                        if (d <= inner) c = 1.0;
                        else {
                            c = (1.0 - d) / (1.0 - inner);
                            c = c * c * (3.0 - 2.0 * c);
                        }
                        float& a = matte[(size_t)(y - win.y1) * w + (x - win.x1)];
                        if (s.erase) a = (float)(a * (1.0 - c));
                        else         a = std::max(a, (float)c);
                    }
                }
            }
        });
    }

    if (getBool(effect, P_INVERT)) {
        for (float& a : matte) a = 1.0f - a;
    }

    bool showMatte = getBool(effect, P_SHOW_MATTE) != 0;
    if (dst.depthBytes == 4)      writeOutput<float, 1>(src, dst, win, matte, showMatte);
    else if (dst.depthBytes == 2) writeOutput<unsigned short, 65535>(src, dst, win, matte, showMatte);
    else                          writeOutput<unsigned char, 255>(src, dst, win, matte, showMatte);

    if (src.handle) gEffectSuite->clipReleaseImage(src.handle);
    gEffectSuite->clipReleaseImage(dst.handle);
    return kOfxStatOK;
}

// ---------------------------------------------------------------------------
// Describe / instances
// ---------------------------------------------------------------------------

static void defineDouble(OfxParamSetHandle set, const char* name, const char* label,
                         double def, double min, double max) {
    OfxPropertySetHandle p = nullptr;
    gParamSuite->paramDefine(set, kOfxParamTypeDouble, name, &p);
    gPropSuite->propSetString(p, kOfxPropLabel, 0, label);
    gPropSuite->propSetDouble(p, kOfxParamPropDefault, 0, def);
    gPropSuite->propSetDouble(p, kOfxParamPropMin, 0, min);
    gPropSuite->propSetDouble(p, kOfxParamPropMax, 0, max);
    gPropSuite->propSetDouble(p, kOfxParamPropDisplayMin, 0, min);
    gPropSuite->propSetDouble(p, kOfxParamPropDisplayMax, 0, max);
    gPropSuite->propSetInt(p, kOfxParamPropAnimates, 0, 0);
}

static OfxStatus describe(OfxImageEffectHandle effect) {
    OfxPropertySetHandle props = effectProps(effect);
    gPropSuite->propSetString(props, kOfxPropLabel, 0, PLUGIN_LABEL);
    gPropSuite->propSetString(props, kOfxImageEffectPluginPropGrouping, 0, PLUGIN_GROUP);
    gPropSuite->propSetString(props, kOfxImageEffectPropSupportedContexts, 0, kOfxImageEffectContextFilter);
    gPropSuite->propSetString(props, kOfxImageEffectPropSupportedPixelDepths, 0, kOfxBitDepthFloat);
    gPropSuite->propSetString(props, kOfxImageEffectPropSupportedPixelDepths, 1, kOfxBitDepthShort);
    gPropSuite->propSetString(props, kOfxImageEffectPropSupportedPixelDepths, 2, kOfxBitDepthByte);
    gPropSuite->propSetInt(props, kOfxImageEffectPropSupportsTiles, 0, 0);
    gPropSuite->propSetInt(props, kOfxImageEffectPropSupportsMultiResolution, 0, 0);
    gPropSuite->propSetString(props, kOfxImageEffectPluginRenderThreadSafety, 0, kOfxImageEffectRenderInstanceSafe);

    if (gDrawSuite) {
        gPropSuite->propSetPointer(props, kOfxImageEffectPluginPropOverlayInteractV2, 0, (void*)interactMain);
    }

#ifdef HB_HAS_CUDA
    gPropSuite->propSetString(props, kOfxImageEffectPropCudaRenderSupported, 0, "true");
    gPropSuite->propSetString(props, kOfxImageEffectPropCudaStreamSupported, 0, "true");
#endif
#ifdef HB_HAS_METAL
    gPropSuite->propSetString(props, kOfxImageEffectPropMetalRenderSupported, 0, "true");
#endif
    return kOfxStatOK;
}

static OfxStatus describeInContext(OfxImageEffectHandle effect) {
    OfxPropertySetHandle clipProps = nullptr;
    gEffectSuite->clipDefine(effect, kOfxImageEffectSimpleSourceClipName, &clipProps);
    gPropSuite->propSetString(clipProps, kOfxImageEffectPropSupportedComponents, 0, kOfxImageComponentRGBA);
    gEffectSuite->clipDefine(effect, kOfxImageEffectOutputClipName, &clipProps);
    gPropSuite->propSetString(clipProps, kOfxImageEffectPropSupportedComponents, 0, kOfxImageComponentRGBA);

    OfxParamSetHandle params = nullptr;
    gEffectSuite->getParamSet(effect, &params);

    defineDouble(params, P_BRUSH_SIZE, "Brush Size", 150, 2, 2000);
    defineDouble(params, P_SOFTNESS, "Softness", 0.5, 0.0, 0.99);

    OfxPropertySetHandle p = nullptr;
    gParamSuite->paramDefine(params, kOfxParamTypeChoice, P_MODE, &p);
    gPropSuite->propSetString(p, kOfxPropLabel, 0, "Mode");
    gPropSuite->propSetString(p, kOfxParamPropChoiceOption, 0, "Paint");
    gPropSuite->propSetString(p, kOfxParamPropChoiceOption, 1, "Erase");
    gPropSuite->propSetInt(p, kOfxParamPropDefault, 0, 0);
    gPropSuite->propSetInt(p, kOfxParamPropAnimates, 0, 0);

    gParamSuite->paramDefine(params, kOfxParamTypeBoolean, P_INVERT, &p);
    gPropSuite->propSetString(p, kOfxPropLabel, 0, "Invert");
    gPropSuite->propSetInt(p, kOfxParamPropDefault, 0, 0);
    gPropSuite->propSetInt(p, kOfxParamPropAnimates, 0, 0);

    gParamSuite->paramDefine(params, kOfxParamTypeBoolean, P_SHOW_MATTE, &p);
    gPropSuite->propSetString(p, kOfxPropLabel, 0, "Show Matte");
    gPropSuite->propSetInt(p, kOfxParamPropDefault, 0, 0);
    gPropSuite->propSetInt(p, kOfxParamPropAnimates, 0, 0);

    gParamSuite->paramDefine(params, kOfxParamTypeInteger, P_NUDGE, &p);
    gPropSuite->propSetInt(p, kOfxParamPropDefault, 0, 0);
    gPropSuite->propSetInt(p, kOfxParamPropSecret, 0, 1);
    gPropSuite->propSetInt(p, kOfxParamPropAnimates, 0, 0);
    gPropSuite->propSetInt(p, kOfxParamPropPersistant, 0, 0);
    gPropSuite->propSetInt(p, kOfxParamPropCanUndo, 0, 0);

    gParamSuite->paramDefine(params, kOfxParamTypeString, P_STROKES, &p);
    gPropSuite->propSetString(p, kOfxPropLabel, 0, "Stroke Data");
    gPropSuite->propSetString(p, kOfxParamPropDefault, 0, "");
    gPropSuite->propSetInt(p, kOfxParamPropSecret, 0, 1);
    gPropSuite->propSetInt(p, kOfxParamPropAnimates, 0, 0);
    gPropSuite->propSetInt(p, kOfxParamPropEvaluateOnChange, 0, 1);

    gParamSuite->paramDefine(params, kOfxParamTypePushButton, P_CLEAR_FRAME, &p);
    gPropSuite->propSetString(p, kOfxPropLabel, 0, "Clear This Frame");
    gParamSuite->paramDefine(params, kOfxParamTypePushButton, P_CLEAR_ALL, &p);
    gPropSuite->propSetString(p, kOfxPropLabel, 0, "Clear All Frames");

    return kOfxStatOK;
}

static OfxStatus createInstance(OfxImageEffectHandle effect) {
    EffectInstance* inst = new EffectInstance();
    gPropSuite->propSetPointer(effectProps(effect), kOfxPropInstanceData, 0, inst);
    reparseFromParam(effect, inst);
    return kOfxStatOK;
}

static OfxStatus destroyInstance(OfxImageEffectHandle effect) {
    EffectInstance* inst = instanceData(effect);
    delete inst;
    gPropSuite->propSetPointer(effectProps(effect), kOfxPropInstanceData, 0, nullptr);
    return kOfxStatOK;
}

static OfxStatus instanceChanged(OfxImageEffectHandle effect, OfxPropertySetHandle inArgs) {
    char* what = nullptr;
    gPropSuite->propGetString(inArgs, kOfxPropName, 0, &what);
    if (!what) return kOfxStatReplyDefault;
    EffectInstance* inst = instanceData(effect);
    if (!inst) return kOfxStatReplyDefault;

    if (strcmp(what, P_CLEAR_ALL) == 0) {
        {
            std::lock_guard<std::mutex> lock(inst->mtx);
            inst->frames.clear();
        }
        setStrokesParam(effect, "");
        return kOfxStatOK;
    }
    if (strcmp(what, P_CLEAR_FRAME) == 0) {
        double time = 0;
        gPropSuite->propGetDouble(inArgs, kOfxPropTime, 0, &time);
        std::string data;
        {
            std::lock_guard<std::mutex> lock(inst->mtx);
            inst->frames.erase((int)floor(time + 0.5));
            data = inst->serialize();
        }
        setStrokesParam(effect, data);
        return kOfxStatOK;
    }
    if (strcmp(what, P_STROKES) == 0) {
        // Covers undo/redo and project load; reparsing our own writes is harmless.
        reparseFromParam(effect, inst);
        return kOfxStatOK;
    }
    return kOfxStatReplyDefault;
}

// ---------------------------------------------------------------------------
// Plugin entry
// ---------------------------------------------------------------------------

static void setHost(OfxHost* host) { gHost = host; }

static OfxStatus pluginMain(const char* action, const void* handle,
                            OfxPropertySetHandle inArgs, OfxPropertySetHandle /*outArgs*/) {
    OfxImageEffectHandle effect = (OfxImageEffectHandle)handle;

    if (strcmp(action, kOfxActionLoad) == 0) {
        if (!gHost) return kOfxStatErrMissingHostFeature;
        gPropSuite   = (OfxPropertySuiteV1*)gHost->fetchSuite(gHost->host, kOfxPropertySuite, 1);
        gEffectSuite = (OfxImageEffectSuiteV1*)gHost->fetchSuite(gHost->host, kOfxImageEffectSuite, 1);
        gParamSuite  = (OfxParameterSuiteV1*)gHost->fetchSuite(gHost->host, kOfxParameterSuite, 1);
        gInteractSuite = (OfxInteractSuiteV1*)gHost->fetchSuite(gHost->host, kOfxInteractSuite, 1);
        gDrawSuite   = (OfxDrawSuiteV1*)gHost->fetchSuite(gHost->host, kOfxDrawSuite, 1);
        if (!gPropSuite || !gEffectSuite || !gParamSuite)
            return kOfxStatErrMissingHostFeature;
        return kOfxStatOK;
    }
    if (strcmp(action, kOfxActionDescribe) == 0)  return describe(effect);
    if (strcmp(action, kOfxImageEffectActionDescribeInContext) == 0) return describeInContext(effect);
    if (strcmp(action, kOfxActionCreateInstance) == 0) return createInstance(effect);
    if (strcmp(action, kOfxActionDestroyInstance) == 0) return destroyInstance(effect);
    if (strcmp(action, kOfxActionInstanceChanged) == 0) return instanceChanged(effect, inArgs);
    if (strcmp(action, kOfxImageEffectActionRender) == 0) return render(effect, inArgs);
    return kOfxStatReplyDefault;
}

static OfxPlugin gPlugin = {
    kOfxImageEffectPluginApi,
    1,
    PLUGIN_ID,
    1,  // major
    2,  // minor
    setHost,
    pluginMain
};

extern "C" {

OfxExport int OfxGetNumberOfPlugins(void) { return 1; }

OfxExport OfxPlugin* OfxGetPlugin(int nth) {
    return (nth == 0) ? &gPlugin : nullptr;
}

} // extern "C"

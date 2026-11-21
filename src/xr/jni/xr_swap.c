/*
 * xr_swap.c - OpenXR swapchain（GL 纹理）实现
 *
 * ALVR 把解码后的画面渲染进这些纹理，我们在 xrEndFrame 里合成到眼镜屏。
 * 纹理必须在与 ALVR 上下文同一 share group 的上下文里创建 —— 我们用自己的
 * 上下文（创建时以 ALVR 上下文为 share_context）来完成。
 */
#include "xr_shim.h"

swapchain_t *xr_swapchain_by_handle(XrSwapchain h) {
    for (swapchain_t *s = g_xr.swapchains; s; s = s->next) {
        if (s->handle == h) return s;
    }
    return NULL;
}

XrResult xrEnumerateSwapchainFormats(XrSession session, uint32_t capacityInput,
                                     uint32_t *countOutput, int64_t *formats) {
    (void)session;
    if (!countOutput) return XR_ERROR_VALIDATION_FAILURE;
    /* 只给 RGBA8：避免 SRGB8_ALPHA8 带来的色彩空间转换问题 */
    *countOutput = 1;
    if (capacityInput == 0) return XR_SUCCESS;
    if (capacityInput < 1) return XR_ERROR_SIZE_INSUFFICIENT;
    formats[0] = GL_RGBA8;
    XLOG("SWAPCHAIN: 上报格式 GL_RGBA8(0x%x)", GL_RGBA8);
    return XR_SUCCESS;
}

XrResult xrCreateSwapchain(XrSession session, const XrSwapchainCreateInfo *createInfo,
                           XrSwapchain *swapchain) {
    (void)session;
    if (!createInfo || !swapchain) return XR_ERROR_VALIDATION_FAILURE;
    if (createInfo->width == 0 || createInfo->height == 0) return XR_ERROR_VALIDATION_FAILURE;

    swapchain_t *sc = calloc(1, sizeof(swapchain_t));
    if (!sc) return XR_ERROR_OUT_OF_MEMORY;
    sc->handle = (XrSwapchain)(uint64_t)(g_xr.next_handle++);
    sc->width = createInfo->width;
    sc->height = createInfo->height;
    sc->format = (int64_t)createInfo->format;
    sc->image_count = XR_SWAPCHAIN_IMAGES;
    sc->next_acquire = 0;
    sc->next = g_xr.swapchains;
    g_xr.swapchains = sc;
    *swapchain = sc->handle;

    XLOG("SWAPCHAIN: 创建 %ux%u fmt=0x%llx usage=0x%llx images=%u", sc->width, sc->height,
         (unsigned long long)createInfo->format, (unsigned long long)createInfo->usageFlags,
         sc->image_count);
    return XR_SUCCESS;
}

XrResult xrDestroySwapchain(XrSwapchain swapchain) {
    swapchain_t **pp = &g_xr.swapchains;
    while (*pp) {
        if ((*pp)->handle == swapchain) {
            swapchain_t *dead = *pp;
            *pp = dead->next;
            if (dead->allocated && g_xr.gl_ready) {
                glDeleteTextures((GLsizei)dead->image_count, dead->textures);
            }
            free(dead);
            return XR_SUCCESS;
        }
        pp = &(*pp)->next;
    }
    return XR_SUCCESS;
}

static bool alloc_textures(swapchain_t *sc) {
    if (sc->allocated) return true;

    /*
     * 关键：不要依赖 g_xr.gl_ready。
     *
     * 眼镜副屏只能"拔下来"才允许启动 VR Activity（华为 VR 服务插着时会拒绝
     * isAbleToLaunchInVr），所以 ALVR 渲染线程建 swapchain 时眼镜通常还不在，
     * gl_ready=false。老代码在这里 return false，导致
     * xrEnumerateSwapchainImages 返回 XR_ERROR_GRAPHICS_DEVICE_INVALID，
     * 客户端 lobby.rs:34 的 .expect() 直接 panic，整个渲染线程死掉 —— 表现就是
     * "进了 VR 但永远连不上服务端"。
     *
     * 其实建纹理不需要窗口：调用方（ALVR 渲染线程）此刻一定有它自己的 EGL 上下文
     * 在 current 上，而我们的上下文是以它为 share_context 建的，两者同一
     * share group，纹理天然共享。所以：
     *   1. 调用方已有 current 上下文 → 就地建纹理，完全不动 MakeCurrent；
     *   2. 我们的上下文已就绪（眼镜在） → 切过去建，再切回来；
     *   3. 两者都没有 → 才是真的没法建。
     */
    EGLContext cur = eglGetCurrentContext();
    EGLSurface prev_draw = eglGetCurrentSurface(EGL_DRAW);
    EGLSurface prev_read = eglGetCurrentSurface(EGL_READ);
    bool switched = false;

    if (cur == EGL_NO_CONTEXT) {
        if (!g_xr.gl_ready || !g_xr.our_context || g_xr.our_surface == EGL_NO_SURFACE) {
            XERR("SWAPCHAIN: 没有可用 GL 上下文（ALVR 没 current，我们的也没就绪）");
            return false;
        }
        if (!eglMakeCurrent(g_xr.egl_display, g_xr.our_surface, g_xr.our_surface,
                            g_xr.our_context)) {
            XERR("SWAPCHAIN: eglMakeCurrent 失败 0x%x", eglGetError());
            return false;
        }
        switched = true;
    }

    GLuint prev_tex = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, (GLint *)&prev_tex);

    glGenTextures((GLsizei)sc->image_count, sc->textures);
    for (uint32_t i = 0; i < sc->image_count; i++) {
        glBindTexture(GL_TEXTURE_2D, sc->textures[i]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei)sc->width, (GLsizei)sc->height, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    glBindTexture(GL_TEXTURE_2D, prev_tex);

    GLenum err = glGetError();
    if (err != GL_NO_ERROR) {
        XERR("SWAPCHAIN: 建纹理 glError=0x%x", err);
    }
    sc->allocated = true;
    XLOG("SWAPCHAIN: 分配纹理 %u 张 (%ux%u) ctx=%s", sc->image_count, sc->width, sc->height,
         switched ? "ours" : "caller");

    if (switched && prev_draw != EGL_NO_SURFACE) {
        eglMakeCurrent(g_xr.egl_display, prev_draw, prev_read, cur);
    }
    return true;
}

XrResult xrEnumerateSwapchainImages(XrSwapchain swapchain, uint32_t capacityInput,
                                    uint32_t *countOutput,
                                    XrSwapchainImageBaseHeader *images) {
    if (!countOutput) return XR_ERROR_VALIDATION_FAILURE;
    swapchain_t *sc = xr_swapchain_by_handle(swapchain);
    if (!sc) return XR_ERROR_HANDLE_INVALID;

    *countOutput = sc->image_count;
    if (capacityInput == 0) return XR_SUCCESS;
    if (capacityInput < sc->image_count) return XR_ERROR_SIZE_INSUFFICIENT;
    if (!alloc_textures(sc)) return XR_ERROR_GRAPHICS_DEVICE_INVALID;

    XrSwapchainImageOpenGLESKHR *arr = (XrSwapchainImageOpenGLESKHR *)images;
    for (uint32_t i = 0; i < sc->image_count; i++) {
        arr[i].type = XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR;
        arr[i].next = NULL;
        arr[i].image = sc->textures[i];
    }
    return XR_SUCCESS;
}

/* 当前处于 acquire 状态的 swapchain / index，供 release 时把 fbo=0 收过来 */
static swapchain_t *s_pending_sc;
static uint32_t s_pending_index;
static long long s_acq_total;

long long xr_swap_acquire_total(void) { return s_acq_total; }

/* 模式 3 的收尾：把 fbo=0 里最后一只眼（eye1）的画面收进它自己的纹理。
 * 在 xrEndFrame 开头、仍在 ALVR 上下文里时调用。 */
void xr_swap_capture_pending(void) {
    if (!s_pending_sc || !s_pending_sc->allocated) return;

    char tag[48];
    snprintf(tag, sizeof(tag), "end-sc%llu#%u",
             (unsigned long long)s_pending_sc->handle, s_pending_index);
    xr_capture_blit_to(s_pending_sc->textures[s_pending_index],
                       (int)s_pending_sc->width, (int)s_pending_sc->height, tag);
}

XrResult xrAcquireSwapchainImage(XrSwapchain swapchain,
                                 const XrSwapchainImageAcquireInfo *acquireInfo,
                                 uint32_t *index) {
    (void)acquireInfo;
    swapchain_t *sc = xr_swapchain_by_handle(swapchain);
    if (!sc || !index) return XR_ERROR_HANDLE_INVALID;

    /*
     * 模式 3：在 acquire 时刻收「上一次」渲染的成果。
     *
     * 时序是这样排的 ——
     *   acquire(左) → 渲 eye0 到 fbo=0 → release(左)
     *   acquire(右) ← 此刻 fbo=0 里还是 eye0 的画面，正好把它收给左眼
     *              → 渲 eye1 到 fbo=0 → release(右)
     *   xrEndFrame  ← 此刻 fbo=0 里是 eye1，收给右眼
     *
     * 所以 acquire 负责收上一只眼，xrEndFrame 负责收最后一只眼。
     */
    if (xr_cfg_capture_alvr() == 3 && s_pending_sc && s_pending_sc->allocated) {
        char tag[48];
        snprintf(tag, sizeof(tag), "acq-sc%llu#%u",
                 (unsigned long long)s_pending_sc->handle, s_pending_index);
        xr_capture_blit_to(s_pending_sc->textures[s_pending_index],
                           (int)s_pending_sc->width, (int)s_pending_sc->height, tag);
    }

    uint32_t i = sc->next_acquire % sc->image_count;
    sc->next_acquire = i + 1;
    sc->acquired[i] = true;
    sc->acquired_index = i;   /* 合成时用这一张 */
    *index = i;
    s_pending_sc = sc;
    s_pending_index = i;
    s_acq_total++;
    return XR_SUCCESS;
}

XrResult xrWaitSwapchainImage(XrSwapchain swapchain,
                              const XrSwapchainImageWaitInfo *waitInfo) {
    (void)waitInfo;
    swapchain_t *sc = xr_swapchain_by_handle(swapchain);
    if (!sc) return XR_ERROR_HANDLE_INVALID;
    return XR_SUCCESS;
}

XrResult xrReleaseSwapchainImage(XrSwapchain swapchain,
                                 const XrSwapchainImageReleaseInfo *releaseInfo) {
    (void)releaseInfo;
    swapchain_t *sc = xr_swapchain_by_handle(swapchain);
    if (!sc) return XR_ERROR_HANDLE_INVALID;

    /*
     * 华为 runtime 的 "acquire-bound framebuffer path"：
     * 应用在 acquire 之后直接把画面渲到 fbo=0（它自己的 window surface），
     * 由运行时在 release 时把这一份收进刚 acquire 的图像里。
     *
     * 我们的等价实现就是在这里 blit 一次。必须用 s_pending_index —— 也就是
     * 应用刚才真正渲进去的那一张 —— 而不是 acquired_index（后者可能已经被
     * 另一只眼的 acquire 覆盖）。
     */
    if (xr_cfg_capture_alvr() == 1 && sc->allocated) {
        static long long n_blit;
        char tag[48];
        snprintf(tag, sizeof(tag), "sc%llu#%u", (unsigned long long)sc->handle, s_pending_index);
        if (xr_capture_blit_to(sc->textures[s_pending_index], (int)sc->width, (int)sc->height, tag)) {
            n_blit++;
            if ((n_blit % 140) == 1) {
                XLOG("CAPTURE: 已把 fbo=0 收进 swapchain 纹理，累计 %lld 次（%s %ux%u）",
                     n_blit, tag, sc->width, sc->height);
            }
        }
    }

    /* 保留 acquired_index 供本帧合成使用 */
    for (uint32_t i = 0; i < sc->image_count; i++) sc->acquired[i] = false;
    return XR_SUCCESS;
}

/*
 * 把 swapchain 当前那张纹理读回来，统计亮度和非黑比例。
 *
 * 这是区分「纹理本身是黑的」和「我们合成画错了」的唯一可靠手段：
 *   - 全黑  → ALVR 没往 swapchain 写东西（它可能直接渲 fbo=0）
 *   - 有内容 → 问题在合成端（采样坐标 / 视口 / 上下文字段）
 *
 * 调用方必须已经 makeCurrent 到 our_context。用临时 FBO 挂纹理读，
 * 读之前 glFinish 一次，跨上下文的 GPU 命令才保证已经落地。
 */
bool xr_swapchain_probe_readback(swapchain_t *sc, const char *tag) {
    if (!sc || !sc->allocated) return false;
    GLuint tex = sc->textures[sc->acquired_index];
    int sw = (int)sc->width, sh = (int)sc->height;
    if (sw <= 0 || sh <= 0) return false;

    GLuint fbo = 0;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (st != GL_FRAMEBUFFER_COMPLETE) {
        XERR("PROBE[%s]: FBO 不完整 0x%x (tex=%u %dx%d)", tag, st, tex, sw, sh);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glDeleteFramebuffers(1, &fbo);
        return false;
    }
    /* 等 ALVR 那边的 GPU 命令全部落地，否则读到的可能是未初始化的空纹理 */
    glFinish();

    unsigned char px[4];
    double sum = 0.0;
    unsigned maxv = 0;
    int nonzero = 0, total = 0;
    for (int gy = 0; gy < 8; gy++) {
        for (int gx = 0; gx < 8; gx++) {
            int x = sw * (gx * 2 + 1) / 16;
            int y = sh * (gy * 2 + 1) / 16;
            glReadPixels(x, y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
            for (int c = 0; c < 3; c++) {
                sum += px[c];
                if ((unsigned)px[c] > maxv) maxv = (unsigned)px[c];
            }
            if (px[0] || px[1] || px[2]) nonzero++;
            total++;
        }
    }
    GLenum err = glGetError();
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &fbo);

    XLOG("PROBE[%s]: tex=%u idx=%u %dx%d mean=%.1f max=%u 非黑=%d/%d glErr=0x%x",
         tag, tex, sc->acquired_index, sw, sh, sum / (total * 3.0), maxv, nonzero, total, err);
    return nonzero > 0;
}

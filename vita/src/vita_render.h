#ifndef __VITA_RENDER_H__
#define __VITA_RENDER_H__

#include <memory>

#include "ultramodern/renderer_context.hpp"

namespace gauntlet {
    namespace vita {
        std::unique_ptr<ultramodern::renderer::RendererContext> create_render_context(uint8_t* rdram, ultramodern::renderer::WindowHandle window_handle, bool developer_mode);
    }
}

#endif

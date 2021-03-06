#pragma once

#include "menu_base.h"

namespace gui {

class sdl1_menu: public menu_base {
public:
    using menu_base::menu_base;

protected:
    void enter() override;
    void leave() override;
    void draw() override;
    size_t page_count() override { return page_size; }

private:
    size_t page_size = 0;
    uint32_t line_height = 0;
    uint32_t value_x = 0;
    uint32_t value_width = 0;
};

}

#pragma once

#include "../Module.hpp"

class ItemTagsModule : public Module {
public:
    ItemTagsModule();
    ~ItemTagsModule() override;

    void onInit() override;
};

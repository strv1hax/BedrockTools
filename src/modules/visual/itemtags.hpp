#pragma once

#include "../Module.hpp"
#include <string>
#include <unordered_map>

class ItemTagsModule : public Module {
public:
    ItemTagsModule();
    ~ItemTagsModule() override;

    void onInit() override;
    void onDisable() override;
};

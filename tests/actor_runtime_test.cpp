#include "actor.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

nc::Actor* childNamed(nc::Actor& parent, const char* name) {
    for (auto& child : parent.children)
        if (child->name == name) return child.get();
    return nullptr;
}

}  // namespace

int main() {
    namespace fs = std::filesystem;
    const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path dir = fs::temp_directory_path() /
                         ("notclon-actor-runtime-" + std::to_string(unique));
    fs::create_directories(dir);

    const fs::path script = dir / "default.lua";
    std::ofstream out(script);
    out << R"lua(
local sleeper
local pose
return Def.ActorFrame{
    Name='Root',
    InitCommand=function(self)
        sleeper=self:GetChild('Sleeper')
        pose=self:GetChild('Pose')
        self:queuecommand('Update')
    end,
    UpdateCommand=function(self)
        sleeper:sleep(100)
        self:x(self:GetX()+1)
        pose:playcommand('Attack')
        self:sleep(0.02):queuecommand('Update')
    end,
    Def.Quad{Name='Sleeper'},
    Def.ActorFrame{
        Name='Pose',
        Def.Sprite{
            Name='Sheet',
            Frames={{Frame=0,Delay=0.04},{Frame=1,Delay=0.04}},
            AttackCommand=function(self) self:setstate(1) end
        }
    }
}
)lua";
    out.close();

    nc::ActorTree tree;
    std::string err;
    if (!tree.load(dir.string(), 0.0, err)) {
        std::fprintf(stderr, "load: %s\n", err.c_str());
        fs::remove_all(dir);
        return 1;
    }
    tree.update(0.11, 0.0);

    nc::Actor* root = tree.root();
    nc::Actor* poseActor = root ? childNamed(*root, "Pose") : nullptr;
    nc::Actor* sheet = poseActor ? childNamed(*poseActor, "Sheet") : nullptr;
    size_t xSegments = 0;
    if (root)
        for (const nc::Seg& seg : root->segs)
            if (seg.prop == nc::PROP_X) ++xSegments;

    const size_t stateKeys = sheet ? sheet->spriteStateKeys.size() : 0;
    fs::remove_all(dir);
    if (xSegments < 5) {
        std::fprintf(stderr,
                     "update pump stalled: only %zu iterations reached x()\n",
                     xSegments);
        return 1;
    }
    if (stateKeys < 5) {
        std::fprintf(stderr,
                     "PlayCommand did not reach the child sprite: %zu state keys\n",
                     stateKeys);
        return 1;
    }
    return 0;
}

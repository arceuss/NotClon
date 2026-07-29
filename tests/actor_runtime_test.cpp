#include "actor.h"

#include <chrono>
#include <cmath>
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
    const nc::Actor aftDefaults;
    if (aftDefaults.aftAlpha || aftDefaults.aftPreserve) {
        std::fprintf(stderr, "ActorFrameTexture defaults differ from SM5.1\n");
        return 1;
    }
    const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path dir = fs::temp_directory_path() /
                         ("notclon-actor-runtime-" + std::to_string(unique));
    fs::create_directories(dir);

    const fs::path script = dir / "default.lua";
    std::ofstream out(script);
    out << R"lua(
local sleeper
GAMESTATE:ApplyModifiers('*-1 stealthglow|0|0|0',1)
GAMESTATE:ApplyModifiers('*-1 stealthglow|1|0|0',1)
GAMESTATE:ApplyModifiers('*2 50 drunk',1)
GAMESTATE:LaunchAttack(0.25,0.5,'*-1 100 flip')
local pose
local queuedPose
return Def.ActorFrame{
    Name='Root',
    InitCommand=function(self)
        sleeper=self:GetChild('Sleeper')
        pose=self:GetChild('Pose')
        queuedPose=self:GetChild('QueuedPose')
        queuedPose:sleep(0.03):queuecommand('Attack')
        SCREENMAN:GetTopScreen():GetChild('PlayerP1')
            :xy(320,240):cmd('zoom,0.75;rotationz,30;visible,0')
        SCREENMAN:GetTopScreen():GetChild('PlayerP2')
            :xy(0,0):linear(2):xy(20,40)
        self:queuecommand('Update')
    end,
    UpdateCommand=function(self)
        sleeper:sleep(100)
        self:x(self:GetX()+1)
        pose:playcommand('Attack')
        self:sleep(0.02):queuecommand('Update')
    end,
    Def.Quad{Name='Sleeper'},
    Def.Quad{
        Name='TweenZoom',
        OnCommand=function(self) self:zoom(1):linear(2):zoom(3) end
    },
    Def.Sprite{
        Name='DepthFlags',
        InitCommand=function(self) self:ztest(1):zwrite(1):texturewrapping(1) end
    },
    Def.Quad{
        Name='VersionDate',
        InitCommand=function(self) self:x(tonumber(GAMESTATE:GetVersionDate())) end
    },
    Def.Quad{
        Name='TapCount',
        InitCommand=function(self) self:x(Plr(1):GetNumTapsInRange(0,60)) end
    },
    Def.Quad{
        Name='GetZValue',
        InitCommand=function(self)
            GAMESTATE:ApplyModifiers('*-1 100 bumpy,*-1 50 movez1',1)
            self:x(GAMESTATE:GetZ(0,0,16))
        end
    },
    Def.Quad{
        Name='CustomTween',
        OnCommand=function(self)
            self:xyz(1,2,3):tween(2,'inOutQuad(%f, 0, 1, 1)'):xyz(11,12,13)
                :glow(.2,.3,.4,.5):SetAwake(true)
        end
    },
    Def.Quad{
        Name='AuxValue',
        OnCommand=function(self) self:aux(2):linear(2):aux(6):x(self:getaux()) end
    },
    Def.Quad{
        Name='CropTween',
        OnCommand=function(self)
            self:cropleft(.1):cropright(.2):croptop(.3):cropbottom(.4)
                :linear(2):cropleft(.5)
        end
    },
    Def.ActorFrameTexture{
        Name='UnnamedAft',InitCommand=function(self)
            self:EnableDepthBuffer(0):EnableAlphaBuffer(0)
                :EnableFloat(0):EnablePreserveTexture(0)
            unnamed_aft=self
        end
    },
    Def.Sprite{
        Name='UnnamedTarget',
        InitCommand=function(self) self:SetTexture(unnamed_aft:GetTexture()) end
    },
    Def.ActorFrameTexture{
        Name='TextureDimensions',
        InitCommand=function(self)
            self:SetWidth(320):SetHeight(240)
            local texture=self:GetTexture()
            self:x(texture:GetImageWidth()):y(texture:GetImageHeight())
        end
    },
    Def.Sprite{Name='AutoLaugh',Texture='laugh 2x1.png'},
    Def.Sprite{Name='AutoSlices',Texture='fuck 32x1.png'},
    Def.Sprite{Name='AutoRows',Texture='fuck 1x24.png'},
    Def.Sprite{
        Name='PausedSheet',Texture='pose_sheet 4x3.png',
        OnCommand=function(self) self:animate(0):texturewrapping(0):setstate(1) end
    },
    Def.ActorFrame{
        Name='Pose',
        Def.Sprite{
            Name='Sheet',
            Frames={{Frame=0,Delay=0.04},{Frame=1,Delay=0.04}},
            AttackCommand=function(self) self:setstate(1) end
        }
    },
    Def.ActorFrame{
        Name='QueuedPose',
        Def.Sprite{
            Name='QueuedSheet',
            AttackCommand=function(self) self:setstate(1) end
        }
    }
}
)lua";
    out.close();

    nc::Chart chart;
    nc::Note chord;
    chord.beat = 10.0;
    chord.frets = 3;
    chart.notes.push_back(chord);
    nc::Note outside;
    outside.beat = 60.0;
    outside.frets = 1;
    chart.notes.push_back(outside);

    nc::ActorTree tree;
    tree.setChart(&chart);
    std::string err;
    if (!tree.load(dir.string(), 0.0, err)) {
        std::fprintf(stderr, "load: %s\n", err.c_str());
        fs::remove_all(dir);
        return 1;
    }
    tree.update(0.11, 0.0);
    nc::PlayerModSnapshot modSnapshot;
    if (!tree.playerModSnapshot(1, modSnapshot) ||
        modSnapshot.target[nc::MOD_DRUNK] != 0.5f ||
        modSnapshot.speed[nc::MOD_DRUNK] != 2.0f) {
        std::fprintf(stderr, "live PlayerOptions target or approach was not exposed\n");
        fs::remove_all(dir);
        return 1;
    }
    std::vector<nc::PlayerModChange> modChanges;
    tree.collectPlayerModChanges(modChanges);
    bool recordedDrunk = false;
    for (const nc::PlayerModChange& change : modChanges)
        if (change.player == 1 && change.mod == nc::MOD_DRUNK &&
            change.target == 0.5f && change.speed == 2.0f)
            recordedDrunk = true;
    if (!recordedDrunk) {
        std::fprintf(stderr, "live PlayerOptions change was not recorded\n");
        fs::remove_all(dir);
        return 1;
    }
    tree.update(0.5, 0.0);
    nc::PlayerModSnapshot attackP1, attackP2;
    if (!tree.playerModSnapshot(1, attackP1) ||
        !tree.playerModSnapshot(2, attackP2) ||
        attackP1.target[nc::MOD_FLIP] != 1.0f ||
        attackP2.target[nc::MOD_FLIP] != 1.0f) {
        std::fprintf(stderr, "LaunchAttack did not activate for both players\n");
        fs::remove_all(dir);
        return 1;
    }
    tree.update(0.8, 0.0);
    if (!tree.playerModSnapshot(1, attackP1) ||
        !tree.playerModSnapshot(2, attackP2) ||
        attackP1.target[nc::MOD_FLIP] != 0.0f ||
        attackP2.target[nc::MOD_FLIP] != 0.0f) {
        std::fprintf(stderr, "LaunchAttack did not restore the underlying options\n");
        fs::remove_all(dir);
        return 1;
    }
    modChanges.clear();
    tree.collectPlayerModChanges(modChanges);
    bool recordedAttackStart = false, recordedAttackEnd = false;
    for (const nc::PlayerModChange& change : modChanges) {
        if (change.player != 1 || change.mod != nc::MOD_FLIP) continue;
        if (change.sec == 0.25 && change.target == 1.0f) recordedAttackStart = true;
        if (change.sec == 0.75 && change.target == 0.0f) recordedAttackEnd = true;
    }
    if (!recordedAttackStart || !recordedAttackEnd) {
        std::fprintf(stderr, "LaunchAttack boundaries were not recorded for export\n");
        fs::remove_all(dir);
        return 1;
    }
    int unknownModWarnings = 0;
    for (const std::string& line : tree.log())
        if (line == "PlayerOptions::FromString ignored unknown mod: stealthglow")
            ++unknownModWarnings;
    if (unknownModWarnings != 1) {
        std::fprintf(stderr, "dynamic unknown mod warnings were not deduplicated\n");
        fs::remove_all(dir);
        return 1;
    }

    const fs::path xmlDir = dir / "xml";
    fs::create_directories(xmlDir);
    std::ofstream sprite(xmlDir / "legacy.sprite");
    sprite << R"sprite([Sprite]
Texture=legacy 2x1.png
Frame0000=0
Delay0000=0.1
Frame0001=1
Delay0001=0.1
OnCommand=x,123
)sprite";
    sprite.close();
    std::ofstream included(xmlDir / "included.xml");
    included << R"xml(<ActorFrame InitCommand="%function(self) child_saw_parent_init=parent_initialized end"><children>
<Layer Type="Quad" Name="Included" />
</children></ActorFrame>)xml";
    included.close();
    std::ofstream xml(xmlDir / "default.xml");
    xml << R"xml(<ActorFrame Name="XmlRoot" InitCommand="%function(self) parent_initialized=true; setup_aft=function(actor) actor:x(99) end; self:SetDrawByZPosition(true); self:SetFarDist(90000) end" OnCommand="%function(self) if child_saw_parent_init and extensionless_actor then self:x(42) else self:x(-1) end end"><children>
<ZZZActor Type="Quad" Name="SortedLast" />
<Special Type="Quad" Name="SortedFirst" Command="y,77" />
<Sprite Name="XmlLaugh" File="laugh 2x1.png" />
<SpriteWithCommand Name="LegacySprite" File="legacy" />
<TTTInclude Name="ExtensionlessXml" Var="extensionless_actor" File="included" />
<UUUSound Type="ActorSound" Name="ThemeSound" File="@THEME:GetPath(EC_SOUNDS,'','Player mine')" />
<VVVLegacy Type="Quad" Name="LegacyXY" OnCommand="xy,0,0;linear,2;xy,20,40" />
<WWWCover Type="Quad" Name="ScaleCover" Frag="test.frag" InitCommand="%function(self) self:GetShader():uniform2f('scale',2,3) end" OnCommand="scaletocover,0,0,640,480" />
<XXXDepth Type="Quad" Name="LegacyDepth" InitCommand="%function(self) GAMESTATE:ApplyModifiers('*-1 50 movez0',1); self:x(GAMESTATE:GetZ(0,0,0)) end" />
<YYYGetn Type="Quad" Name="LegacyGetn" InitCommand="%function(self) local t={} for i=1,5 do table.insert(t,i) end t[5]=nil self:x(table.getn(t)) end" />
<ZZYQueueCurrent Type="Quad" Name="LegacyQueueCurrent" OnCommand="%function(self) self:x(10); self:sleep(1); self:queuecommand('After') end" AfterCommand="addx,5" />
<ZZXExpression Type="Quad" Name="LegacyExpression" OnCommand="xy,SCREEN_WIDTH/2,SCREEN_HEIGHT/2;z,SCREEN_LEFT+SCREEN_TOP+3;halign,0.25;valign,0.75;clearzbuffer,1;texcoordvelocity,0.25,-0.5;glowshift;effectdelay,0.5" />
<ZZWFunctionRef Type="Quad" Name="LegacyFunctionRef" OnCommand="%setup_aft" />
<ZZVStretch Type="Quad" Name="LegacyStretch" OnCommand="stretchto,0,0,SCREEN_WIDTH,SCREEN_HEIGHT" />
<ZZUTheme Type="Quad" Name="LegacyTheme" InitCommand="%function(self) self:x(type(THEME:GetPath(EC_GRAPHICS,'','x')) == 'string' and 1 or -1) end" />
<ZZTAft Type="ActorFrameTexture" Name="LegacyAft" />
<ZZSProxy Type="ActorProxy" Name="LegacyPlayerProxy" InitCommand="%function(self) if P1 then self:SetTarget(P1:GetChild('NoteField')) end end" />
</children></ActorFrame>)xml";
    xml.close();
    nc::ActorTree xmlTree;
    xmlTree.setDisplaySize(854, 480);
    if (!xmlTree.load(xmlDir.string(), 0.0, err)) {
        std::fprintf(stderr, "XML load: %s\n", err.c_str());
        fs::remove_all(dir);
        return 1;
    }
    nc::Actor* xmlLaugh = xmlTree.root() ? childNamed(*xmlTree.root(), "XmlLaugh") : nullptr;
    nc::Actor* legacySprite = xmlTree.root() ? childNamed(*xmlTree.root(), "LegacySprite") : nullptr;
    nc::Actor* extensionlessXml = xmlTree.root()
                                ? childNamed(*xmlTree.root(), "ExtensionlessXml") : nullptr;
    nc::Actor* themeSound = xmlTree.root()
                          ? childNamed(*xmlTree.root(), "ThemeSound") : nullptr;
    nc::Actor* legacyXY = xmlTree.root()
                        ? childNamed(*xmlTree.root(), "LegacyXY") : nullptr;
    nc::Actor* scaleCover = xmlTree.root()
                          ? childNamed(*xmlTree.root(), "ScaleCover") : nullptr;
    nc::Actor* legacyDepth = xmlTree.root()
                           ? childNamed(*xmlTree.root(), "LegacyDepth") : nullptr;
    nc::Actor* legacyGetn = xmlTree.root()
                          ? childNamed(*xmlTree.root(), "LegacyGetn") : nullptr;
    nc::Actor* legacyQueueCurrent = xmlTree.root()
                                  ? childNamed(*xmlTree.root(), "LegacyQueueCurrent") : nullptr;
    nc::Actor* legacyExpression = xmlTree.root()
                                ? childNamed(*xmlTree.root(), "LegacyExpression") : nullptr;
    nc::Actor* legacyFunctionRef = xmlTree.root()
                                 ? childNamed(*xmlTree.root(), "LegacyFunctionRef") : nullptr;
    nc::Actor* legacyStretch = xmlTree.root()
                             ? childNamed(*xmlTree.root(), "LegacyStretch") : nullptr;
    nc::Actor* legacyTheme = xmlTree.root()
                           ? childNamed(*xmlTree.root(), "LegacyTheme") : nullptr;
    nc::Actor* legacyAft = xmlTree.root()
                         ? childNamed(*xmlTree.root(), "LegacyAft") : nullptr;
    nc::Actor* legacyPlayerProxy = xmlTree.root()
                                 ? childNamed(*xmlTree.root(), "LegacyPlayerProxy") : nullptr;
    if (!xmlLaugh || xmlLaugh->sheetCols != 2 || xmlLaugh->sheetRows != 1 ||
        xmlLaugh->spriteFrames.size() != 2) {
        std::fprintf(stderr, "XML 2x1 filename did not create two default states\n");
        fs::remove_all(dir);
        return 1;
    }
    if (!xmlTree.root() || xmlTree.root()->children.size() != 17 ||
        xmlTree.root()->children.front()->name != "SortedFirst" ||
        xmlTree.root()->children.back()->name != "SortedLast") {
        std::fprintf(stderr, "XML ActorFrame children were not sorted by tag name\n");
        fs::remove_all(dir);
        return 1;
    }
    const nc::Seg* legacyX = nullptr;
    const nc::Seg* legacyY = nullptr;
    if (legacyXY) {
        for (const nc::Seg& seg : legacyXY->segs) {
            if (seg.prop == nc::PROP_X && seg.to[0] == 20.0f) legacyX = &seg;
            if (seg.prop == nc::PROP_Y && seg.to[0] == 40.0f) legacyY = &seg;
        }
    }
    if (!legacyX || !legacyY || legacyX->t0 != legacyY->t0 ||
        legacyX->t1 != legacyY->t1 || legacyX->t1 - legacyX->t0 != 2.0f) {
        std::fprintf(stderr, "legacy tweened xy did not move both axes together\n");
        fs::remove_all(dir);
        return 1;
    }
    bool covered = false;
    if (scaleCover) {
        bool x = false, y = false, zx = false, zy = false;
        for (const nc::Seg& seg : scaleCover->segs) {
            if (seg.prop == nc::PROP_X && seg.to[0] == 320.0f) x = true;
            if (seg.prop == nc::PROP_Y && seg.to[0] == 240.0f) y = true;
            if (seg.prop == nc::PROP_ZOOMX && seg.to[0] == 640.0f) zx = true;
            if (seg.prop == nc::PROP_ZOOMY && seg.to[0] == 640.0f) zy = true;
        }
        covered = x && y && zx && zy;
    }
    if (!covered) {
        std::fprintf(stderr, "scaletocover did not centre and uniformly cover the rectangle\n");
        fs::remove_all(dir);
        return 1;
    }
    if (!legacyDepth || fabsf(legacyDepth->base.x - 32.0f) > 0.001f) {
        std::fprintf(stderr, "legacy movez0 was not treated as column zero\n");
        fs::remove_all(dir);
        return 1;
    }
    if (!legacyGetn || legacyGetn->base.x != 5.0f) {
        std::fprintf(stderr, "legacy table.getn lost its stored size after a hole\n");
        fs::remove_all(dir);
        return 1;
    }
    xmlTree.update(1.1, 0.0);
    float queuedX = 0.0f;
    if (legacyQueueCurrent) {
        for (const nc::Seg& seg : legacyQueueCurrent->segs)
            if (seg.prop == nc::PROP_X) queuedX = seg.to[0];
    }
    if (queuedX != 15.0f) {
        std::fprintf(stderr, "legacy queuecommand did not start from current state\n");
        fs::remove_all(dir);
        return 1;
    }
    bool expressionX = false, expressionY = false, expressionZ = false;
    bool expressionHAlign = false, expressionVAlign = false, expressionClearZ = false;
    if (legacyExpression) {
        for (const nc::Seg& seg : legacyExpression->segs) {
            if (seg.prop == nc::PROP_X && seg.to[0] == 427.0f) expressionX = true;
            if (seg.prop == nc::PROP_Y && seg.to[0] == 240.0f) expressionY = true;
            if (seg.prop == nc::PROP_Z && seg.to[0] == 3.0f) expressionZ = true;
            if (seg.prop == nc::PROP_HALIGN && seg.to[0] == -0.5f) expressionHAlign = true;
            if (seg.prop == nc::PROP_VALIGN && seg.to[0] == 0.5f) expressionVAlign = true;
            if (seg.prop == nc::PROP_CLEARZ && seg.to[0] == 1.0f) expressionClearZ = true;
        }
    }
    bool unsupportedActorMethod = false;
    for (const std::string& line : xmlTree.log())
        if (line.find("unsupported Actor method") != std::string::npos)
            unsupportedActorMethod = true;
    if (!expressionX || !expressionY || !expressionZ ||
        !expressionHAlign || !expressionVAlign || !expressionClearZ ||
        !legacyExpression || legacyExpression->effect.kind != nc::Effect::GlowShift ||
        legacyExpression->effect.delay != 0.5f ||
        legacyExpression->texCoordVelX != 0.25f ||
        legacyExpression->texCoordVelY != -0.5f || unsupportedActorMethod) {
        std::fprintf(stderr, "legacy Actor method expressions were not applied\n");
        fs::remove_all(dir);
        return 1;
    }
    bool functionRefX = false;
    if (legacyFunctionRef) {
        for (const nc::Seg& seg : legacyFunctionRef->segs)
            if (seg.prop == nc::PROP_X && seg.to[0] == 99.0f) functionRefX = true;
    }
    if (!functionRefX) {
        std::fprintf(stderr, "legacy percent function reference was not invoked\n");
        fs::remove_all(dir);
        return 1;
    }
    bool stretchX = false, stretchY = false, stretchZX = false, stretchZY = false;
    if (legacyStretch) {
        for (const nc::Seg& seg : legacyStretch->segs) {
            if (seg.prop == nc::PROP_X && seg.to[0] == 427.0f) stretchX = true;
            if (seg.prop == nc::PROP_Y && seg.to[0] == 240.0f) stretchY = true;
            if (seg.prop == nc::PROP_ZOOMX && seg.to[0] == 854.0f) stretchZX = true;
            if (seg.prop == nc::PROP_ZOOMY && seg.to[0] == 480.0f) stretchZY = true;
        }
    }
    if (!stretchX || !stretchY || !stretchZX || !stretchZY) {
        std::fprintf(stderr, "legacy stretchto did not fill the requested rectangle\n");
        fs::remove_all(dir);
        return 1;
    }
    if (!legacyTheme || legacyTheme->base.x != 1.0f) {
        std::fprintf(stderr, "legacy THEME:GetPath compatibility was unavailable\n");
        fs::remove_all(dir);
        return 1;
    }
    if (!scaleCover || scaleCover->shaderFrag.find("test.frag") == std::string::npos ||
        scaleCover->shaderUniforms.size() != 1 ||
        scaleCover->shaderUniforms[0].name != "scale" ||
        scaleCover->shaderUniforms[0].components != 2 ||
        scaleCover->shaderUniforms[0].value[0] != 2.0f ||
        scaleCover->shaderUniforms[0].value[1] != 3.0f) {
        std::fprintf(stderr, "actor shader attributes or retained uniforms were lost\n");
        fs::remove_all(dir);
        return 1;
    }
    if (!extensionlessXml || !childNamed(*extensionlessXml, "Included")) {
        std::fprintf(stderr, "extensionless XML actor was not loaded\n");
        fs::remove_all(dir);
        return 1;
    }
    if (!themeSound || !themeSound->file.empty()) {
        std::fprintf(stderr, "ActorSound expression was treated as a texture\n");
        fs::remove_all(dir);
        return 1;
    }
    bool xmlGlobalsWorked = false;
    if (xmlTree.root()) {
        for (const nc::Seg& seg : xmlTree.root()->segs)
            if (seg.prop == nc::PROP_X && seg.to[0] == 42.0f)
                xmlGlobalsWorked = true;
    }
    if (!xmlGlobalsWorked) {
        std::fprintf(stderr, "nested XML root command or Var global was discarded\n");
        fs::remove_all(dir);
        return 1;
    }
    const std::string* spriteOn = legacySprite
                                ? legacySprite->findCommand("OnCommand") : nullptr;
    if (!spriteOn || *spriteOn != "x,123") {
        std::fprintf(stderr, ".sprite OnCommand was not imported\n");
        fs::remove_all(dir);
        return 1;
    }
    nc::Actor* sortedFirst = xmlTree.root()
                           ? childNamed(*xmlTree.root(), "SortedFirst") : nullptr;
    const std::string* aliasedOn = sortedFirst
                                 ? sortedFirst->findCommand("OnCommand") : nullptr;
    if (!aliasedOn || *aliasedOn != "y,77") {
        std::fprintf(stderr, "legacy Command attribute was not aliased to OnCommand\n");
        fs::remove_all(dir);
        return 1;
    }

    nc::Actor* root = tree.root();
    nc::Actor* poseActor = root ? childNamed(*root, "Pose") : nullptr;
    nc::Actor* queuedPoseActor = root ? childNamed(*root, "QueuedPose") : nullptr;
    nc::Actor* sheet = poseActor ? childNamed(*poseActor, "Sheet") : nullptr;
    nc::Actor* queuedSheet = queuedPoseActor ? childNamed(*queuedPoseActor, "QueuedSheet") : nullptr;
    nc::Actor* laugh = root ? childNamed(*root, "AutoLaugh") : nullptr;
    nc::Actor* slices = root ? childNamed(*root, "AutoSlices") : nullptr;
    nc::Actor* rows = root ? childNamed(*root, "AutoRows") : nullptr;
    nc::Actor* pausedSheet = root ? childNamed(*root, "PausedSheet") : nullptr;
    nc::Actor* tweenZoom = root ? childNamed(*root, "TweenZoom") : nullptr;
    nc::Actor* depthFlags = root ? childNamed(*root, "DepthFlags") : nullptr;
    nc::Actor* versionDate = root ? childNamed(*root, "VersionDate") : nullptr;
    nc::Actor* tapCount = root ? childNamed(*root, "TapCount") : nullptr;
    nc::Actor* getZValue = root ? childNamed(*root, "GetZValue") : nullptr;
    nc::Actor* customTween = root ? childNamed(*root, "CustomTween") : nullptr;
    nc::Actor* auxValue = root ? childNamed(*root, "AuxValue") : nullptr;
    nc::Actor* cropTween = root ? childNamed(*root, "CropTween") : nullptr;
    nc::Actor* unnamedAft = root ? childNamed(*root, "UnnamedAft") : nullptr;
    nc::Actor* unnamedTarget = root ? childNamed(*root, "UnnamedTarget") : nullptr;
    nc::Actor* textureDimensions = root ? childNamed(*root, "TextureDimensions") : nullptr;
    size_t xSegments = 0;
    if (root)
        for (const nc::Seg& seg : root->segs)
            if (seg.prop == nc::PROP_X) ++xSegments;

    const size_t stateKeys = sheet ? sheet->spriteStateKeys.size() : 0;
    const size_t queuedStateKeys = queuedSheet ? queuedSheet->spriteStateKeys.size() : 0;
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
    if (queuedStateKeys != 1) {
        std::fprintf(stderr,
                     "ActorFrame queuecommand did not propagate to its child: %zu state keys\n",
                     queuedStateKeys);
        return 1;
    }
    if (!laugh || laugh->sheetCols != 2 || laugh->sheetRows != 1 ||
        laugh->spriteFrames.size() != 2) {
        std::fprintf(stderr, "2x1 filename did not create two default states\n");
        return 1;
    }
    if (!slices || slices->sheetCols != 32 || slices->sheetRows != 1 ||
        slices->spriteFrames.size() != 32) {
        std::fprintf(stderr, "32x1 filename did not create 32 default states\n");
        return 1;
    }
    if (!rows || rows->sheetCols != 1 || rows->sheetRows != 24 ||
        rows->spriteFrames.size() != 24) {
        std::fprintf(stderr, "1x24 filename did not create 24 default states\n");
        return 1;
    }
    if (!pausedSheet || pausedSheet->sheetCols != 4 || pausedSheet->sheetRows != 3 ||
        pausedSheet->spriteFrames.size() != 12 || pausedSheet->spriteAnimate ||
        pausedSheet->textureWrapping ||
        pausedSheet->spriteStateKeys.size() != 1 ||
        pausedSheet->spriteStateKeys.front().state != 1) {
        std::fprintf(stderr, "numeric false did not pause the selected sprite state\n");
        return 1;
    }
    const nc::Seg* zoomX = nullptr;
    const nc::Seg* zoomY = nullptr;
    if (tweenZoom) {
        for (const nc::Seg& seg : tweenZoom->segs) {
            if (seg.prop == nc::PROP_ZOOMX && seg.to[0] == 3.0f) zoomX = &seg;
            if (seg.prop == nc::PROP_ZOOMY && seg.to[0] == 3.0f) zoomY = &seg;
        }
    }
    if (!zoomX || !zoomY || zoomX->t0 != zoomY->t0 || zoomX->t1 != zoomY->t1 ||
        zoomX->t1 - zoomX->t0 != 2.0f) {
        std::fprintf(stderr, "zoom did not tween both axes together\n");
        return 1;
    }
    if (!depthFlags || !depthFlags->base.zTest || !depthFlags->base.zWrite ||
        !depthFlags->textureWrapping) {
        std::fprintf(stderr, "Lua depth or texture-wrapping methods were ignored\n");
        return 1;
    }
    if (!versionDate || versionDate->base.x < 20190804.0f) {
        std::fprintf(stderr, "GAMESTATE:GetVersionDate did not select the modern API path\n");
        return 1;
    }
    if (!tapCount || tapCount->base.x != 2.0f) {
        std::fprintf(stderr, "Player:GetNumTapsInRange did not count chart taps\n");
        return 1;
    }
    const float expectedZ = 40.0f * sinf(1.0f) + 32.0f;
    if (!getZValue || fabsf(getZValue->base.x - expectedZ) > 0.001f) {
        std::fprintf(stderr, "GAMESTATE:GetZ did not return the modded note depth\n");
        return 1;
    }
    const nc::Seg* cropLeft = nullptr;
    const nc::Seg* cropRight = nullptr;
    const nc::Seg* cropTop = nullptr;
    const nc::Seg* cropBottom = nullptr;
    if (cropTween) for (const nc::Seg& seg : cropTween->segs) {
        if (seg.prop == nc::PROP_CROPLEFT && seg.to[0] == .5f) cropLeft = &seg;
        if (seg.prop == nc::PROP_CROPRIGHT && seg.to[0] == .2f) cropRight = &seg;
        if (seg.prop == nc::PROP_CROPTOP && seg.to[0] == .3f) cropTop = &seg;
        if (seg.prop == nc::PROP_CROPBOTTOM && seg.to[0] == .4f) cropBottom = &seg;
    }
    if (!cropLeft || !cropRight || !cropTop || !cropBottom ||
        cropLeft->t1 - cropLeft->t0 != 2.0f) {
        std::fprintf(stderr, "crop methods were not retained as tween state\n");
        return 1;
    }
    const nc::Seg* customX = nullptr;
    const nc::Seg* customY = nullptr;
    const nc::Seg* customZ = nullptr;
    const nc::Seg* customGlow = nullptr;
    if (customTween) for (const nc::Seg& seg : customTween->segs) {
        if (seg.prop == nc::PROP_X && seg.to[0] == 11.0f) customX = &seg;
        if (seg.prop == nc::PROP_Y && seg.to[0] == 12.0f) customY = &seg;
        if (seg.prop == nc::PROP_Z && seg.to[0] == 13.0f) customZ = &seg;
        if (seg.prop == nc::PROP_GLOWALPHA && seg.n == 4) customGlow = &seg;
    }
    if (!customX || !customY || !customZ || !customGlow ||
        customX->t0 != customY->t0 || customX->t0 != customZ->t0 ||
        customX->t1 != customY->t1 || customX->t1 != customZ->t1 ||
        customX->ease != nc::Ease::InOutQuad ||
        customGlow->to[0] != .2f || customGlow->to[1] != .3f ||
        customGlow->to[2] != .4f || customGlow->to[3] != .5f) {
        std::fprintf(stderr,
                     "NotITG xyz, tween, or glow state was not retained: "
                     "actor=%p x=%p y=%p z=%p glow=%p ease=%d rgba=%g,%g,%g,%g\n",
                     static_cast<void*>(customTween),
                     static_cast<const void*>(customX), static_cast<const void*>(customY),
                     static_cast<const void*>(customZ), static_cast<const void*>(customGlow),
                     customX ? int(customX->ease) : -1,
                     customGlow ? customGlow->to[0] : -1.0f,
                     customGlow ? customGlow->to[1] : -1.0f,
                     customGlow ? customGlow->to[2] : -1.0f,
                     customGlow ? customGlow->to[3] : -1.0f);
        for (const std::string& line : tree.log())
            std::fprintf(stderr, "actor log: %s\n", line.c_str());
        return 1;
    }
    const nc::Seg* auxTween = nullptr;
    const nc::Seg* auxRead = nullptr;
    if (auxValue) for (const nc::Seg& seg : auxValue->segs) {
        if (seg.prop == nc::PROP_AUX && seg.to[0] == 6.0f) auxTween = &seg;
        if (seg.prop == nc::PROP_X && seg.to[0] == 6.0f) auxRead = &seg;
    }
    if (!auxValue || !auxRead || !auxTween ||
        auxTween->t1 - auxTween->t0 != 2.0f) {
        std::fprintf(stderr, "Actor aux/getaux was not retained as tween state\n");
        return 1;
    }
    if (!unnamedAft || unnamedAft->aftDepth || unnamedAft->aftAlpha ||
        unnamedAft->aftFloat || unnamedAft->aftPreserve ||
        !unnamedTarget || unnamedTarget->textureTarget != unnamedAft) {
        std::fprintf(stderr, "SetTexture discarded an unnamed AFT texture object\n");
        return 1;
    }
    if (unnamedAft->aftCapturePrevious || !legacyAft ||
        !legacyAft->aftCapturePrevious) {
        std::fprintf(stderr, "Lua and legacy XML AFT capture contracts were conflated\n");
        return 1;
    }
    if (!legacyPlayerProxy || !legacyPlayerProxy->proxyTarget ||
        legacyPlayerProxy->proxyTarget->playerField != 1) {
        std::fprintf(stderr, "legacy player global did not resolve lazily\n");
        return 1;
    }
    if (!xmlTree.root()->drawByZPosition || xmlTree.root()->farDist != 90000.0f) {
        std::fprintf(stderr, "ActorFrame draw order or far distance was not retained\n");
        return 1;
    }
    if (!textureDimensions || textureDimensions->base.x != 320.0f ||
        textureDimensions->base.y != 240.0f) {
        std::fprintf(stderr, "AFT image dimensions were not numeric or did not match its image\n");
        return 1;
    }
    nc::ActorState player;
    nc::Actor* noteField = childNamed(tree.plrProxy(0), "NoteField");
    if (tree.plrProxy(0).playerField != 0 || !noteField ||
        noteField->playerField != 1) {
        std::fprintf(stderr, "Player and NoteField were not kept as distinct targets\n");
        return 1;
    }
    if (!tree.playerState(1, 0.0, 0.0, player) ||
        player.x != 320.0f || player.y != 240.0f ||
        player.zoomX != 0.75f || player.zoomY != 0.75f ||
        player.rotZ != 30.0f || !player.hidden) {
        std::fprintf(stderr, "Player state did not retain its complete actor transform\n");
        return 1;
    }
    if (!tree.playerState(2, 1.0, 0.0, player) ||
        player.x != 10.0f || player.y != 20.0f) {
        std::fprintf(stderr, "Lua tweened xy did not move both axes together\n");
        return 1;
    }
    return 0;
}

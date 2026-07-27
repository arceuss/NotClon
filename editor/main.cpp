// NotClon editor -- previews a chart and edits its modchart.
//
// The point of this existing as its own binary is that it draws frames with
// nc::Renderer, the exact code notclon.exe encodes with. What you scrub past
// here is what lands in the MP4; there is no "editor renderer".
//
// Layout: the render target is shown 1:1 (scaled to fit) in a viewport panel,
// with a transport under it and two side panels -- the entry list, and the
// live value of every knob at the playhead. Mods are added the way OpenITG
// states them: a tick, a percent, and an approach rate.

#include "renderer.h"
#include "modfile.h"
#include "stems.h"
#include "actor.h"
#include "background.h"
#include "audio.h"

#include <SDL3/SDL.h>

#include "imgui.h"
#include "backends/imgui_impl_sdl3.h"
#include "backends/imgui_impl_opengl3.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace {

// SDL's file dialogs are asynchronous; the callback fires on the main thread
// during SDL_PumpEvents, so a plain pointer to a std::string is safe here.
struct DialogResult {
    std::string path;
    bool ready = false;
};

void SDLCALL onFilePicked(void* userdata, const char* const* files, int) {
    auto* r = static_cast<DialogResult*>(userdata);
    if (files && files[0]) { r->path = files[0]; r->ready = true; }
}

const SDL_DialogFileFilter MOD_FILTER[] = {
    {"NotClon modchart", "ncmod"},
    {"All files", "*"},
};

const SDL_DialogFileFilter CHART_FILTER[] = {
    {"Clone Hero chart", "chart"},
    {"All files", "*"},
};

std::string baseName(const std::string& p) {
    size_t i = p.find_last_of("/\\");
    return i == std::string::npos ? p : p.substr(i + 1);
}

std::string dirName(const std::string& p) {
    size_t i = p.find_last_of("/\\");
    return i == std::string::npos ? std::string(".") : p.substr(0, i);
}

// Moonscraper's snap ladder -- these are the divisions a CH charter already
// thinks in. The value is the denominator of a whole note, so 4 is a quarter
// note and step = resolution * 4 / N ticks.
const int SNAPS[] = {1, 2, 4, 8, 12, 16, 24, 32, 48, 64, 96, 192};
const int SNAP_COUNT = int(sizeof(SNAPS) / sizeof(SNAPS[0]));

}  // namespace

int main(int argc, char** argv) {
    // The editor starts on a blank document. --dir is a convenience, not a
    // requirement: with no chart it comes up on an empty highway and you open
    // one from the UI, which is also what happens when a .ncmod names its own.
    std::string startDir, modPath, assetDir;
    int outW = 1280, outH = 720;
    // Same defaults and spelling as notclon.exe: the preview is only worth
    // trusting if the background it draws is the one the encoder would.
    std::vector<std::string> bgShaderArgs;
    std::vector<std::string> fxShaderArgs;
    double bgScale = 0.5;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() { return std::string(argv[++i]); };
        if      (a == "--dir")  startDir = next();
        else if (a == "--mods") modPath = next();
        else if (a == "--w")    outW = atoi(next().c_str());
        else if (a == "--h")    outH = atoi(next().c_str());
        else if (a == "--assets") assetDir = next();
        else if (a == "--bgshader") bgShaderArgs.push_back(next());
        else if (a == "--fxshader") fxShaderArgs.push_back(next());
        else if (a == "--bgscale")  bgScale = atof(next().c_str());
    }

    nc::Chart chart;
    nc::ModDoc doc;
    std::string chartDir;
    bool haveChart = false;
    int  lastTick = chart.resolution * 16;

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO)) {
        fprintf(stderr, "SDL_Init: %s%c", SDL_GetError(), 10);
        return 1;
    }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_COMPATIBILITY);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

    SDL_Window* win = SDL_CreateWindow("NotClon editor", 1600, 900,
                                       SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
    if (!win) { fprintf(stderr, "SDL_CreateWindow: %s%c", SDL_GetError(), 10); return 1; }
    SDL_GLContext ctx = SDL_GL_CreateContext(win);
    SDL_GL_MakeCurrent(win, ctx);
    SDL_GL_SetSwapInterval(1);
    if (!nc_load_gl()) return 1;

    nc::Renderer R;
    if (!R.init(outW, outH, nc::nc_findAssets(assetDir))) return 1;
    R.buildHitTimes(chart);   // empty until a chart is opened

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    // Deliberately NOT NavEnableKeyboard: it makes ImGui claim WantCaptureKeyboard
    // permanently and binds the arrow keys to widget navigation, which is exactly
    // where the transport shortcuts live.
    ImGui::StyleColorsDark();
    ImGui_ImplSDL3_InitForOpenGL(win, ctx);
    ImGui_ImplOpenGL3_Init("#version 330");

    nce::Audio audio;
    bool haveAudio = false;

    nc::RenderOpts opt;
    opt.doc = &doc;

    // The background and actor layers the encoder builds (src/main.cpp), built
    // here too -- without them the editor is not previewing what notclon.exe
    // renders, which is the whole reason the two binaries share one Renderer.
    // unique_ptr rather than by value because opening a second chart must
    // discard the first one's trees, textures and ffmpeg pipes, and neither
    // class has a clear(); re-seating the pointer is the clear.
    std::unique_ptr<nc::Background> bg;
    std::unique_ptr<nc::ActorLayer> actors;
    bool noBg = false, noActors = false;

    // --bgshader with no chart open is the shader-authoring case: the layer
    // stack has to exist before a .sm does. openChart rebuilds it from here.
    if (!bgShaderArgs.empty()) {
        bg = std::make_unique<nc::Background>();
        for (const auto& fp : bgShaderArgs) bg->addShader(fp);
        if (bg->empty()) bg.reset();
    }

    double tick = 0.0;
    bool playing = false, wasPlaying = false;
    float rate = 1.0f;
    int   selected = -1;
    std::string status = "no chart -- Open chart";

    // Everything that depends on which chart is loaded lives here, so opening
    // one from the UI and opening one named by a .ncmod take the same path.
    auto openChart = [&](const std::string& dir) -> bool {
        nc::Chart c;
        if (!c.load(dir + "/notes.chart")) return false;
        chart = c;
        chartDir = dir;
        haveChart = true;
        lastTick = chart.notes.empty() ? chart.resolution * 16
                                       : chart.notes.back().tick + chart.resolution * 8;
        R.buildHitTimes(chart);
        // Audio, discovered the way CH does it: BassAudioManager.cs probes 14
        // fixed stem names (guitar/bass/rhythm/vocals*/drums*/keys/song/crowd)
        // over {ogg,mp3,wav} and plays EVERY stem it finds simultaneously --
        // MusicStream is parsed and never used for playback. All present
        // stems are mixed; MusicStream is a NotClon-only fallback for folders
        // that predate this. Audio remains optional.
        haveAudio = audio.loadMix(nc::findAudioStems(dir, chart.musicStream));

        // Actor + background layers, discovered exactly as src/main.cpp does:
        // one .sm beside the chart feeds both -- its #FG/#BGCHANGES folder
        // entries are actor trees, its media entries are the background.
        // Rebuilt from scratch every open; a chart with neither leaves both
        // null, which is the same skipped pass the encoder has.
        std::string smBeside;
        {
            std::error_code ec;
            for (const auto& e : std::filesystem::directory_iterator(dir, ec))
                if (e.path().extension() == ".sm") {
                    smBeside = e.path().generic_string(); break;
                }
        }
        actors = std::make_unique<nc::ActorLayer>();
        if (!smBeside.empty()) {
            std::string aerr;
            actors->loadFromSm(smBeside, dir, aerr);
        }
        if (actors->empty()) actors.reset();
        bg = std::make_unique<nc::Background>();
        if (!smBeside.empty()) bg->loadFromSm(smBeside, dir, float(bgScale));
        for (const auto& fp : bgShaderArgs) bg->addShader(fp);
        if (bg->empty()) bg.reset();

        doc.rebuild(chart);          // tick->second mapping just changed
        tick = 0.0; playing = false;
        return true;
    };

    if (!startDir.empty() && !openChart(startDir))
        status = "cannot load " + startDir + "/notes.chart";

    if (!modPath.empty()) {
        if (doc.load(modPath)) {
            // A modchart names the chart it was written against, so opening one
            // is enough to get back to where you were.
            if (!doc.chartDir.empty() && doc.chartDir != chartDir)
                openChart(doc.chartDir);
            doc.rebuild(chart);
            status = "loaded " + baseName(modPath);
        }
    }
    if (haveChart && doc.chartDir.empty()) doc.chartDir = chartDir;

    // "Add mod" form state
    int   formMod = nc::MOD_DRUNK;
    float formPercent = 50.0f;
    float formApproach = 1.0f;
    int   formLen = 0;      // ticks; 0 = holds until something else changes it
    int   addMode = 1;      // 0 exact tick, 1 snap grid, 2 nearest note
    int   snapIdx = 3;      // 1/8

    DialogResult dlgOpen, dlgSave, dlgChart;
    bool hoveringPreview = false;
    Uint64 prevCounter = SDL_GetPerformanceCounter();

    bool running = true;
    while (running) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            ImGui_ImplSDL3_ProcessEvent(&ev);
            if (ev.type == SDL_EVENT_QUIT) running = false;
            if (ev.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED &&
                ev.window.windowID == SDL_GetWindowID(win)) running = false;
        }

        const Uint64 now = SDL_GetPerformanceCounter();
        const double dt = double(now - prevCounter) / double(SDL_GetPerformanceFrequency());
        prevCounter = now;

        if (dlgChart.ready) {
            dlgChart.ready = false;
            const std::string dir = dirName(dlgChart.path);
            if (openChart(dir)) {
                doc.chartDir = dir;
                selected = -1;
                status = "opened " + baseName(dir);
            } else {
                status = "cannot load " + dlgChart.path;
            }
        }
        if (dlgOpen.ready) {
            dlgOpen.ready = false;
            modPath = dlgOpen.path;
            if (doc.load(modPath)) {
                if (!doc.chartDir.empty() && doc.chartDir != chartDir) {
                    if (!openChart(doc.chartDir))
                        status = "modchart names a chart that will not load: " + doc.chartDir;
                }
                doc.rebuild(chart);
                selected = -1;
                if (haveChart && doc.chartDir.empty()) doc.chartDir = chartDir;
                status = "loaded " + baseName(modPath);
            } else {
                status = "cannot load " + baseName(modPath);
            }
        }
        if (dlgSave.ready) {
            dlgSave.ready = false;
            modPath = dlgSave.path;
            if (modPath.size() < 6 || modPath.compare(modPath.size() - 6, 6, ".ncmod") != 0)
                modPath += ".ncmod";
            if (doc.save(modPath)) status = "saved " + baseName(modPath);
        }

        // Playback works in seconds and converts back, so tempo changes scrub
        // at the right speed instead of at a fixed ticks-per-second. The clock
        // is the audio device wherever it can be -- see audio.h.
        if (playing) {
            audio.pump();
            double sec;
            // No `tickToSec(tick) >= 0` term here any more. It used to hand the
            // clock from frame delta to the device the instant the song proper
            // began, and the two did not agree: the device had been playing
            // from second zero the whole time the visual clock was crawling
            // through the negative lead-in, so the playhead lurched forward by
            // the length of that lead-in. The device now plays the lead-in as
            // silence and is the clock throughout.
            if (audio.ok() && !audio.exhausted())
                sec = audio.time();
            else
                sec = chart.tickToSec(tick) + dt * rate;
            tick = chart.secToBeat(sec) * chart.resolution;
            if (tick >= lastTick) { tick = lastTick; playing = false; }
        }
        // Anything that moves the playhead after this point is the user
        // jumping, and a jump has to cut the audio rather than be chased.
        const double advancedTick = tick;

        // ---- render the frame the encoder would produce ---------------------
        // Hot reload: an edited .frag rebuilds in place, keeping the last good
        // program if it fails to compile. Editor only -- a shader must not
        // change under an encode.
        if (bg) bg->reloadIfChanged();
        // The null pointer IS the "off" switch, so there is no RenderOpts::noBg
        // to keep in sync with it. Re-seated every frame: a pointer store, and
        // the alternative is change-detection on two checkboxes plus openChart.
        R.setBackground(!noBg && bg ? bg.get() : nullptr);
        R.setActorLayer(!noActors && actors ? actors.get() : nullptr);
        R.drawFrame(chart, tick / double(chart.resolution), opt, R.postFbo());

        int winW = 0, winH = 0;
        SDL_GetWindowSizeInPixels(win, &winW, &winH);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport(0, 0, winW, winH);
        glClearColor(0.09f, 0.09f, 0.11f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();

        const float SIDE = 380.0f;
        const float rightW = 300.0f;

        // ---- viewport -------------------------------------------------------
        ImGui::SetNextWindowPos(ImVec2(SIDE, 0));
        ImGui::SetNextWindowSize(ImVec2(float(winW) - SIDE - rightW, float(winH)));
        ImGui::Begin("Preview", nullptr,
                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse);
        {
            const ImVec2 avail = ImGui::GetContentRegionAvail();
            const float scale = std::min(avail.x / float(outW),
                                         (avail.y - 92.0f) / float(outH));
            const ImVec2 sz(float(outW) * scale, float(outH) * scale);
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (avail.x - sz.x) * 0.5f);
            // uv flipped in y: GL textures are bottom-up.
            ImGui::Image((ImTextureID)(intptr_t)R.postTex(), sz,
                         ImVec2(0, 1), ImVec2(1, 0));
            hoveringPreview = ImGui::IsItemHovered();

            ImGui::Spacing();
            const double beat = tick / double(chart.resolution);
            if (ImGui::Button(playing ? "Pause" : "Play", ImVec2(72, 0))) playing = !playing;
            ImGui::SameLine();
            if (ImGui::Button("|<", ImVec2(36, 0))) { tick = 0; playing = false; }
            ImGui::SameLine();
            ImGui::SetNextItemWidth(110);
            ImGui::InputFloat("rate", &rate, 0.0f, 0.0f, "%.2f");
            if (rate < 0.05f) rate = 0.05f;
            if (rate > 4.0f)  rate = 4.0f;
            ImGui::SameLine();
            ImGui::Text("1/%d", SNAPS[snapIdx]);
            ImGui::SameLine();
            ImGui::TextDisabled("(<- ->)");
            ImGui::SameLine();
            ImGui::Text("tick %.0f   beat %.3f   %.2fs   bpm %.1f",
                        tick, beat, chart.tickToSec(tick), chart.bpmAt(beat));

            float ft = float(tick);
            ImGui::SetNextItemWidth(-1);
            if (ImGui::SliderFloat("##scrub", &ft, 0.0f, float(lastTick), "")) {
                tick = ft;
                playing = false;
            }

            // Section markers double as the seek bar's labels.
            ImGui::SetNextItemWidth(-1);
            if (ImGui::BeginCombo("##section", "jump to section")) {
                for (const auto& s : chart.sections)
                    if (ImGui::Selectable(s.name.c_str())) {
                        tick = s.beat * chart.resolution;
                        playing = false;
                    }
                ImGui::EndCombo();
            }
        }
        ImGui::End();

        // ---- left: the modchart --------------------------------------------
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2(SIDE, float(winH)));
        ImGui::Begin("Modchart", nullptr,
                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse);
        {
            // The chart is a property of the document, not of the command
            // line: a .ncmod records the folder it was authored against, so
            // opening either one gets you back to the same session.
            if (ImGui::Button("Open chart")) {
                SDL_ShowOpenFileDialog(onFilePicked, &dlgChart, win,
                                       CHART_FILTER, 2, nullptr, false);
            }
            ImGui::SameLine();
            ImGui::TextDisabled("%s", haveChart ? baseName(chartDir).c_str() : "(none)");

            if (ImGui::Button("Open")) {
                SDL_ShowOpenFileDialog(onFilePicked, &dlgOpen, win,
                                       MOD_FILTER, 2, nullptr, false);
            }
            ImGui::SameLine();
            if (ImGui::Button("Save")) {
                if (modPath.empty())
                    SDL_ShowSaveFileDialog(onFilePicked, &dlgSave, win,
                                           MOD_FILTER, 2, nullptr);
                else if (doc.save(modPath)) status = "saved " + baseName(modPath);
            }
            ImGui::SameLine();
            if (ImGui::Button("Save As")) {
                SDL_ShowSaveFileDialog(onFilePicked, &dlgSave, win,
                                       MOD_FILTER, 2, nullptr);
            }
            ImGui::TextDisabled("%s", status.c_str());

            ImGui::SeparatorText("Add at playhead");

            // Snapping is what makes this usable: a mod that fires a few ticks
            // off the beat it was meant for reads as a glitch, and eyeballing a
            // scrub bar cannot hit an exact tick.
            int addTick = int(tick + 0.5);
            if (addMode == 1) {
                const double step = double(chart.resolution) * 4.0 / double(SNAPS[snapIdx]);
                addTick = int(floor(tick / step + 0.5) * step + 0.5);
            } else if (addMode == 2 && !chart.notes.empty()) {
                int best = chart.notes[0].tick;
                for (const auto& n : chart.notes)
                    if (abs(n.tick - addTick) < abs(best - addTick)) best = n.tick;
                addTick = best;
            }

            ImGui::SetNextItemWidth(150);
            if (ImGui::BeginCombo("mod", nc::modName(formMod))) {
                for (int i = 0; i < nc::MOD_COUNT; ++i) {
                    if (i == nc::MOD_MOVEX)      ImGui::SeparatorText("playfield");
                    if (i == nc::MOD_ABERRATION) ImGui::SeparatorText("post");
                    char lbl[64];
                    snprintf(lbl, sizeof lbl, "%s%s", nc::modName(i),
                             nc::modIsStub(i) ? " (stub)" : "");
                    if (ImGui::Selectable(lbl, i == formMod)) formMod = i;
                }
                ImGui::EndCombo();
            }
            ImGui::SetNextItemWidth(150);
            ImGui::DragFloat("percent", &formPercent, 1.0f, -400.0f, 400.0f, "%.0f%%");
            ImGui::SetNextItemWidth(150);
            ImGui::DragFloat("approach", &formApproach, 0.05f, -1.0f, 20.0f, "*%.2f");
            ImGui::SameLine();
            ImGui::TextDisabled("(?)");
            if (ImGui::BeginItemTooltip()) {
                ImGui::TextUnformatted(
                    "Full swing per second, as in OpenITG's *n.\n"
                    "*1 reaches 100% in one second, *4 in a quarter second.\n"
                    "*0 or below snaps instantly (the *-1 idiom).");
                ImGui::EndTooltip();
            }
            // Typed, not dragged. A tick count is a number you know -- 192 for
            // a beat, 768 for a bar -- and dragging to it four ticks at a time
            // is busywork. Percent and approach stay drags because those you
            // tune by feel against the preview.
            ImGui::SetNextItemWidth(150);
            ImGui::InputInt("len", &formLen, 0, 0);
            if (formLen < 0) formLen = 0;
            if (formLen > lastTick) formLen = lastTick;
            ImGui::SameLine();
            ImGui::TextDisabled("(?)");
            if (ImGui::BeginItemTooltip()) {
                ImGui::TextUnformatted(
                    "How long the entry stays in force, in ticks.\n"
                    "0 holds until something else changes the knob.\n"
                    "Above 0 it is an OpenITG attack: the knob reverts on\n"
                    "its own when the window closes, so you do not have to\n"
                    "schedule it back.");
                ImGui::EndTooltip();
            }
            ImGui::SetNextItemWidth(150);
            ImGui::Combo("place at", &addMode, "exact tick\0snap grid\0nearest note\0");
            ImGui::Text("-> tick %d  (beat %.3f)", addTick,
                        addTick / double(chart.resolution));
            if (ImGui::Button("Add", ImVec2(-1, 0))) {
                doc.add(chart, addTick, formMod, formPercent / 100.0f, formApproach, formLen);
                status = "added";
            }
            // Turning a mod off *in the chart* means scheduling it back to
            // zero -- there is no "delete from here on". This is the one-click
            // form of OpenITG's `*-1 0 <mod>`.
            if (ImGui::Button("Off here (0%)", ImVec2(-1, 0))) {
                doc.add(chart, addTick, formMod, 0.0f, formApproach);
                status = std::string("zeroed ") + nc::modName(formMod);
            }

            // ---- selected entry ---------------------------------------------
            ImGui::SeparatorText("Selected");
            if (selected >= 0 && selected < int(doc.entries.size())) {
                nc::ModEntry e = doc.entries[selected];
                bool changed = false;

                ImGui::SetNextItemWidth(150);
                if (ImGui::BeginCombo("mod##sel", nc::modName(e.mod))) {
                    for (int i = 0; i < nc::MOD_COUNT; ++i) {
                        char lbl[64];
                        snprintf(lbl, sizeof lbl, "%s%s", nc::modName(i),
                                 nc::modIsStub(i) ? " (stub)" : "");
                        if (ImGui::Selectable(lbl, i == e.mod)) {
                            e.mod = i; changed = true;
                        }
                    }
                    ImGui::EndCombo();
                }
                // Dragging these re-evaluates every frame, so the preview
                // follows the slider -- that is the whole point of tuning here
                // rather than deleting and re-adding.
                float pct = e.percent * 100.0f;
                ImGui::SetNextItemWidth(150);
                if (ImGui::DragFloat("percent##sel", &pct, 1.0f, -400.0f, 400.0f, "%.0f%%")) {
                    e.percent = pct / 100.0f; changed = true;
                }
                ImGui::SetNextItemWidth(150);
                if (ImGui::DragFloat("approach##sel", &e.approach, 0.05f, -1.0f, 20.0f, "*%.2f"))
                    changed = true;
                ImGui::SetNextItemWidth(150);
                if (ImGui::InputInt("tick##sel", &e.tick, 0, 0)) {
                    if (e.tick < 0) e.tick = 0;
                    if (e.tick > lastTick) e.tick = lastTick;
                    changed = true;
                }
                ImGui::SetNextItemWidth(150);
                if (ImGui::InputInt("len##sel", &e.len, 0, 0)) {
                    if (e.len < 0) e.len = 0;
                    if (e.len > lastTick) e.len = lastTick;
                    changed = true;
                }
                if (e.len > 0)
                    ImGui::TextDisabled("reverts at tick %d (beat %.3f)", e.tick + e.len,
                                        (e.tick + e.len) / double(chart.resolution));

                if (ImGui::Checkbox("enabled", &e.enabled)) changed = true;
                ImGui::SameLine();
                ImGui::TextDisabled("(?)");
                if (ImGui::BeginItemTooltip()) {
                    ImGui::TextUnformatted(
                        "Mutes this entry while keeping its values, for A/B-ing.\n"
                        "It is saved as a '#!' comment line.\n\n"
                        "To turn a mod off in the chart itself, add an entry at\n"
                        "0% instead -- that is what OpenITG does.");
                    ImGui::EndTooltip();
                }

                if (changed) {
                    // set() re-sorts, so a tick drag can move the row out from
                    // under the selection. Follow it by identity, not index.
                    doc.set(chart, size_t(selected), e);
                    for (int i = 0; i < int(doc.entries.size()); ++i)
                        if (doc.entries[i].tick == e.tick && doc.entries[i].mod == e.mod &&
                            doc.entries[i].percent == e.percent) { selected = i; break; }
                }

                if (ImGui::Button("Go to")) {
                    tick = doc.entries[selected].tick;
                    playing = false;
                }
                ImGui::SameLine();
                if (ImGui::Button("Duplicate here")) {
                    doc.add(chart, addTick, e.mod, e.percent, e.approach, e.len);
                    status = "duplicated";
                }
                ImGui::SameLine();
                if (ImGui::Button("Delete")) {
                    doc.erase(chart, size_t(selected));
                    selected = -1;
                }
            } else {
                ImGui::TextDisabled("select an entry below");
            }

            ImGui::SeparatorText("Entries");

            if (ImGui::BeginTable("entries", 5,
                                  ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders |
                                  ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp)) {
                ImGui::TableSetupColumn("tick", ImGuiTableColumnFlags_WidthFixed, 58);
                ImGui::TableSetupColumn("mod");
                ImGui::TableSetupColumn("%", ImGuiTableColumnFlags_WidthFixed, 46);
                ImGui::TableSetupColumn("*", ImGuiTableColumnFlags_WidthFixed, 40);
                ImGui::TableSetupColumn("len", ImGuiTableColumnFlags_WidthFixed, 46);
                ImGui::TableSetupScrollFreeze(0, 1);
                ImGui::TableHeadersRow();

                for (int i = 0; i < int(doc.entries.size()); ++i) {
                    const nc::ModEntry& e = doc.entries[i];
                    ImGui::TableNextRow();
                    // Highlight whatever the playhead has already passed.
                    if (e.tick <= tick)
                        ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0,
                                               IM_COL32(40, 70, 40, 120));
                    ImGui::TableNextColumn();
                    ImGui::PushID(i);
                    if (ImGui::Selectable(std::to_string(e.tick).c_str(), selected == i,
                                          ImGuiSelectableFlags_SpanAllColumns))
                        selected = i;
                    ImGui::PopID();
                    if (!e.enabled) ImGui::BeginDisabled();
                    ImGui::TableNextColumn(); ImGui::TextUnformatted(nc::modName(e.mod));
                    ImGui::TableNextColumn(); ImGui::Text("%g", e.percent * 100.0f);
                    ImGui::TableNextColumn(); ImGui::Text("%g", e.approach);
                    ImGui::TableNextColumn();
                    if (e.len > 0) ImGui::Text("%d", e.len); else ImGui::TextDisabled("-");
                    if (!e.enabled) ImGui::EndDisabled();
                }
                ImGui::EndTable();
            }
        }
        ImGui::End();

        // ---- right: resolved values at the playhead -------------------------
        ImGui::SetNextWindowPos(ImVec2(float(winW) - rightW, 0));
        ImGui::SetNextWindowSize(ImVec2(rightW, float(winH)));
        ImGui::Begin("Values", nullptr,
                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse);
        {
            ImGui::SeparatorText("At playhead");
            // MOD_SLOTS, not MOD_COUNT: the packed evalAt writes every slot
            // including the bg.<name> range, and a short buffer is a stack
            // smash.
            float v[nc::MOD_SLOTS];
            doc.evalAt(chart, tick, v);
            for (int i = 0; i < nc::MOD_COUNT; ++i) {
                if (i == nc::MOD_MOVEX)      ImGui::SeparatorText("playfield");
                if (i == nc::MOD_ABERRATION) ImGui::SeparatorText("post");
                const bool active = v[i] != nc::modDefault(i);
                if (!active) ImGui::BeginDisabled();
                ImGui::Text("%-11s %7.1f%%", nc::modName(i), v[i] * 100.0f);
                if (!active) ImGui::EndDisabled();
            }
            // Only the bg.<name> slots this document actually drives: an
            // undriven slot is never handed to the shader at all, so listing
            // it at 0% would claim an effect it does not have.
            const unsigned bgUsed = doc.bgUsedMask();
            if (bgUsed) {
                ImGui::SeparatorText("background");
                for (int i = 0; i < nc::MAX_BG_UNIFORMS; ++i)
                    if ((bgUsed >> i) & 1u)
                        ImGui::Text("%-11s %7.1f%%",
                                    nc::modName(nc::MOD_BG_BASE + i),
                                    v[nc::MOD_BG_BASE + i] * 100.0f);
            }

            ImGui::SeparatorText("Render");
            ImGui::Checkbox("playfield only", &opt.playfield);
            ImGui::Checkbox("no post", &opt.noPost);
            ImGui::Checkbox("no mods", &opt.noMods);
            ImGui::Checkbox("no bot", &opt.noBot);
            ImGui::Checkbox("no background", &noBg);
            ImGui::Checkbox("no actors", &noActors);
            ImGui::SetNextItemWidth(-1);
            ImGui::InputFloat("speed", &opt.noteSpeed, 0.0f, 0.0f, "%.1f");
            if (opt.noteSpeed < 1.0f)  opt.noteSpeed = 1.0f;
            if (opt.noteSpeed > 40.0f) opt.noteSpeed = 40.0f;

            // ---- background layers ------------------------------------------
            // What loaded, and why a .frag is not showing. A shader that fails
            // to compile keeps its last good program, so without this panel a
            // typo looks like "my edit did nothing".
            ImGui::SeparatorText("Background");
            if (!bg) {
                ImGui::TextDisabled("none (no .sm beside the chart)");
            } else {
                for (size_t i = 0; i < bg->layerCount(); ++i)
                    ImGui::BulletText("%s", bg->layerDesc(i).c_str());
                if (ImGui::Button("Clear log")) bg->clearLog();
                if (ImGui::BeginChild("bglog", ImVec2(0, 120), ImGuiChildFlags_Borders)) {
                    for (const auto& m : bg->log())
                        ImGui::TextWrapped("%s", m.c_str());
                }
                ImGui::EndChild();
            }

            ImGui::SeparatorText("Chart");
            ImGui::TextWrapped("%s - %s", chart.artist.c_str(), chart.name.c_str());
            ImGui::Text("%zu notes, res %d", chart.notes.size(), chart.resolution);
            if (haveAudio) ImGui::Text("audio %.1fs", audio.duration());
            else           ImGui::TextDisabled("no audio (song.ogg?)");
            ImGui::Text("%.1f fps", double(ImGui::GetIO().Framerate));
        }
        ImGui::End();

        // ---- keyboard -------------------------------------------------------
        // WantTextInput, not WantCaptureKeyboard: only a live text field should
        // swallow the transport keys.
        if (!ImGui::GetIO().WantTextInput) {
            if (ImGui::IsKeyPressed(ImGuiKey_Space)) playing = !playing;

            // Left/Right pick the snap division, Up/Down step by it. Up is
            // forward, matching Moonscraper -- the highway travels toward you,
            // so scrolling "up" the board is scrolling later in the song.
            if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)  && snapIdx > 0) --snapIdx;
            if (ImGui::IsKeyPressed(ImGuiKey_RightArrow) && snapIdx < SNAP_COUNT - 1) ++snapIdx;

            const double step = double(chart.resolution) * 4.0 / double(SNAPS[snapIdx]);
            int dir = 0;
            if (ImGui::IsKeyPressed(ImGuiKey_UpArrow))   dir = 1;
            if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) dir = -1;
            // The wheel does the same thing, but only over the preview -- the
            // entry table needs its own scrolling.
            if (hoveringPreview && ImGui::GetIO().MouseWheel != 0.0f)
                dir = ImGui::GetIO().MouseWheel > 0.0f ? 1 : -1;

            if (dir) {
                // Land on the grid rather than offsetting from wherever the
                // playhead happened to stop, so repeated steps stay exact.
                const double q = tick / step;
                double n = (dir > 0) ? floor(q + 1e-6) + 1.0 : ceil(q - 1e-6) - 1.0;
                tick = n * step;
                if (tick < 0) tick = 0;
                if (tick > lastTick) tick = lastTick;
                playing = false;
            }
        }

        audio.setRate(rate);
        // A jump, or the frame play was pressed, both need the device refilled
        // from the new position -- and the clock only becomes valid once it is.
        if (tick != advancedTick || (playing && !wasPlaying))
            audio.seek(chart.tickToSec(tick));
        audio.setPlaying(playing);
        if (playing && !wasPlaying) audio.pump();
        wasPlaying = playing;

        ImGui::Render();
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        SDL_GL_SwapWindow(win);
    }

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    SDL_GL_DestroyContext(ctx);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}

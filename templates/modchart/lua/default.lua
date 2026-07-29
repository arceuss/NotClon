-- A NotClon/SM5.1 actor tree.
--
-- Drop this folder next to your notes.chart and load it with:
--
--     notclon --dir "charts/My Song" --actor lua
--
-- or, if your song came from a .sm, let its #FGCHANGES load it automatically.
--
-- ActorDef tables use Def.* constructors. Commands are Lua functions whose
-- actor methods return self, so calls can be chained. A tween (linear,
-- decelerate, ...) applies to the state changes after it; sleep starts a new
-- tween segment.
--
-- Coordinates use StepMania's 480-high, aspect-correct virtual screen, with
-- (0,0) at the top-left (640x480 at 4:3, 854x480 at 16:9). SCREEN_WIDTH,
-- SCREEN_HEIGHT, SCREEN_CENTER_X and SCREEN_CENTER_Y are available.

return Def.ActorFrame{
    -- A plain quad. InitCommand runs during construction; OnCommand runs after
    -- every actor has completed InitCommand.
    Def.Quad{
        InitCommand=function(self)
            self:x(SCREEN_CENTER_X):y(60):zoomto(240, 6):diffuse(1, 1, 1, 0)
        end,
        OnCommand=function(self)
            self:sleep(1):linear(0.4):diffusealpha(1)
            self:sleep(2):linear(0.8):diffusealpha(0)
        end,
    },

    -- Put an image in this folder to enable this example. Filename grids such
    -- as "mygraphic 2x1.png" automatically become SM sprite animation states.
    --[[
    Def.Sprite{
        Texture="mygraphic.png",
        OnCommand=function(self)
            self:x(-100):y(SCREEN_CENTER_Y)
            self:decelerate(0.8):x(SCREEN_CENTER_X)
            self:bounce():effectmagnitude(0, -12, 0)
                :effectclock("bgm"):effectperiod(1)
            self:sleep(8):linear(0.5):diffusealpha(0)
        end,
    },
    ]]

    -- Anything that broadcasts "Flash" runs this command.
    Def.Quad{
        InitCommand=function(self)
            self:x(SCREEN_CENTER_X):y(SCREEN_CENTER_Y)
                :zoomto(SCREEN_WIDTH, SCREEN_HEIGHT):diffuse(1, 1, 1, 0)
        end,
        FlashMessageCommand=function(self)
            self:finishtweening():diffusealpha(0.6)
                :linear(0.35):diffusealpha(0)
        end,
    },

    Def.Quad{
        InitCommand=function(self)
            self:visible(false)
        end,
        OnCommand=function(self)
            local dir = GAMESTATE:GetCurrentSong():GetSongDir()
            local beat = GAMESTATE:GetSongBeat()
            MESSAGEMAN:Broadcast("Flash")
        end,
    },
}

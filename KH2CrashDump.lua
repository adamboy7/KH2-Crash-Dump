LUAGUI_NAME = "KH2 Crash Dump"
LUAGUI_AUTH = "adamboy7"
LUAGUI_DESC = "Writes a crash dump and a crash log when the game crashes"

-- Settings ------------------------------------------------------------------------------------
-- Files go in the game's install folder (next to the exe):
--   KH2Crash.log                  one record per crash: what crashed, where, registers, stack
--   KH2Crash-<date>-<time>.dmp    the minidump (WinDbg, Visual Studio, Python "minidump")
local PREFIX = "KH2Crash"
local KEEP = 5         -- dumps to keep; older ones are deleted when a new one is written (0 = all)
local FULL = false     -- true: include all of the game's memory (gigabytes, slow to write)
-- ----------------------------------------------------------------------------------------------

local cd
local frames = 0

function _OnInit()
    if ENGINE_TYPE ~= "BACKEND" then return end
    local ok, mod = pcall(require, "kh2crashdump")
    if not ok then
        ConsolePrint("[KH2CrashDump] kh2crashdump.dll did not load: " .. tostring(mod), 3)
        return
    end
    local where, err = mod.arm({ prefix = PREFIX, keep = KEEP, full = FULL })
    if not where then
        ConsolePrint("[KH2CrashDump] not armed: " .. tostring(err), 3)
        return
    end
    cd = mod
    ConsolePrint("[KH2CrashDump] armed: " .. where .. "-*.dmp")
end

function _OnFrame()
    if not cd then return end
    frames = frames + 1
    if frames % 300 == 0 then cd.keep() end   -- in case another mod replaced the crash filter
end

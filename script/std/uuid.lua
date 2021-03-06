--[[

  Generate an unique identifier for each connection.

]]--

local uuid, fp, L = require"uuid", require"utils.fp", require"utils.lambda"
local fold, last = fp.fold, fp.last

local urandom = io.open("/dev/urandom")
uuid.randomseed(last(fold.vz(L"_1 * 256 + _2", 0, urandom:read(4):byte())) % 0x7FFFFFFF)
urandom:close()

local function adduuid(info) info.ci.extra.uuid = uuid() end
spaghetti.addhook("clientconnect", adduuid, true)
spaghetti.addhook("botjoin", adduuid, true)

return { find = function(ciuuid)
  local l1, l2 = server.clients, server.connects
  for i = 0, l1:length() - 1 do if l1[i].extra.uuid == ciuuid then return l1[i] end end
  for i = 0, l2:length() - 1 do if l2[i].extra.uuid == ciuuid then return l2[i] end end
end }

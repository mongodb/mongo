-- prevent wireshark loading this file as a plugin
if not _G['pcapng_test_gen'] then return end


local block = require "blocks"
local input = require "input"


local test = {
    category    = 'difficult',
    description = "Multible SHB sections, one with invalid version number",
}


local timestamp = UInt64(0x64ca47aa, 0x0004c397)

function test:compile()
    local idb0 = block.IDB(0, input.linktype.ETHERNET, 0, "eth0")

    self.blocks = {
        block.SHB("my computer", "linux", "pcap_writer.lua")
            :addOption('comment', self.testname .. " SHB-0"),
        idb0,

        block.EPB( idb0, input:getData(1), timestamp ),

        block.SHB("my computer", "linux", "pcap_writer.lua")
            :addOption('comment', self.testname .. " SHB-1")
            :setVersion(2, 0),
        idb0,

        block.EPB( idb0, input:getData(2), timestamp ),

        block.SHB("my computer", "linux", "pcap_writer.lua")
            :addOption('comment', self.testname .. " SHB-2"),
        idb0,

        block.EPB( idb0, input:getData(3), timestamp ),
    }
end


return test

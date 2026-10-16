/**
 * Tests the helpers in jstests/libs/network/host_ipaddr.js that discover the host's external IPv4
 * address.
 */

import {describe, it} from "jstests/libs/mochalite.js";
import {
    get_ipaddr,
    parseInterfaceListOutput,
    parseRouteSrcOutput,
} from "jstests/libs/network/host_ipaddr.js";

describe("parseRouteSrcOutput", function () {
    it("returns the source address", function () {
        assert.eq(parseRouteSrcOutput("src 10.122.7.177\n"), "10.122.7.177");
    });

    it("returns null when 'ip' is unavailable or has no route", function () {
        assert.eq(parseRouteSrcOutput(""), null);
    });
});

describe("parseInterfaceListOutput", function () {
    it("parses modern ifconfig output", function () {
        const output =
            "        inet 10.122.7.177  netmask 255.255.240.0  broadcast 10.122.15.255\n" +
            "        inet 172.17.0.1  netmask 255.255.0.0  broadcast 172.17.255.255\n";
        assert.eq(parseInterfaceListOutput(output), "10.122.7.177");
    });

    it("parses older ifconfig output with 'addr:' prefixes", function () {
        const output = "          inet addr:10.0.0.5  Bcast:10.0.0.255  Mask:255.255.255.0\n";
        assert.eq(parseInterfaceListOutput(output), "10.0.0.5");
    });

    it("parses 'ip addr' output", function () {
        const output = "    inet 10.0.0.5/24 brd 10.0.0.255 scope global eth0\n";
        assert.eq(parseInterfaceListOutput(output), "10.0.0.5");
    });

    it("parses 'hostname -I' output rewritten as inet lines", function () {
        assert.eq(parseInterfaceListOutput("inet 10.0.0.5\ninet 172.17.0.1\n"), "10.0.0.5");
    });

    it("returns the first listed address, even if it belongs to a docker bridge", function () {
        // This is why get_ipaddr() prefers the default route's source address.
        const output =
            "        inet 172.21.0.1  netmask 255.255.0.0  broadcast 172.21.255.255\n" +
            "        inet 10.122.7.177  netmask 255.255.240.0  broadcast 10.122.15.255\n";
        assert.eq(parseInterfaceListOutput(output), "172.21.0.1");
    });

    it("returns null when no address is listed", function () {
        assert.eq(parseInterfaceListOutput(""), null);
    });
});

describe("get_ipaddr", function () {
    it("returns a non-loopback IPv4 address that a mongod can bind to", function () {
        const ip = get_ipaddr();
        assert(/^\d+\.\d+\.\d+\.\d+$/.test(ip), "Expected an IPv4 address", {ip});
        assert(!ip.startsWith("127."), "Expected a non-loopback address", {ip});

        // MongoRunner waits for startup over localhost, so bind to it too. Binding fails, and
        // mongod exits, if 'ip' is not assigned to a local interface.
        const conn = MongoRunner.runMongod({bind_ip: "127.0.0.1," + ip});
        try {
            const remoteConn = new Mongo(ip + ":" + conn.port);
            assert.commandWorked(remoteConn.adminCommand({ping: 1}));
        } finally {
            MongoRunner.stopMongod(conn);
        }
    });
});

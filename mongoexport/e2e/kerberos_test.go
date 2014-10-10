package mongoexport_e2e_test

import (
	. "github.com/smartystreets/goconvey/convey"
	"os/exec"
	"strings"
	"testing"
)

func TestKerberosConnection(t *testing.T) {
	Convey("Should be able to export with Keberos authentication", t, func() {
		// Note: mongoexport should be in PATH. In addition, on Linux, you should
		// have already run `kinit`
		command := exec.Command("sh", "-c",
			`mongoexport`+
				` -h ldaptest.10gen.cc`+ // Hostname
				` -u drivers@LDAPTEST.10GEN.CC`+ // Principal
				` --authenticationMechanism GSSAPI`+ // Auth mechanism for Kerberos
				` --authenticationDatabase "$external"`+ // Auth database for Kerberos
				` -d kerberos`+ // Export from db named 'kerberos'
				` -c test`, // And collection named test
			// (see wiki.mongodb.com/display/DH/Testing+Kerberos)
		)

		stdout, err := command.CombinedOutput()
		So(err, ShouldBeNil)
		stdoutString := string(stdout)

		outputLines := strings.Split(strings.TrimSpace(stdoutString), "\n")
		So(len(outputLines), ShouldEqual, 2)

		jsonOutput := "{\"_id\":{\"$oid\":\"528fb35afb3a8030e2f643c3\"}," +
			"\"authenticated\":\"yeah\",\"kerberos\":true}"
		So(outputLines[0], ShouldEqual, jsonOutput)
		So(outputLines[1], ShouldEqual, "exported 1 record")
	})
}

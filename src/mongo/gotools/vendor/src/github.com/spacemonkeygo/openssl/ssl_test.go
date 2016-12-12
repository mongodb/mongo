// Copyright (C) 2014 Space Monkey, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

package openssl

import (
	"bytes"
	"crypto/rand"
	"crypto/tls"
	"io"
	"io/ioutil"
	"net"
	"sync"
	"testing"
	"time"

	"github.com/spacemonkeygo/openssl/utils"
)

var (
	certBytes = []byte(`-----BEGIN CERTIFICATE-----
MIIDxDCCAqygAwIBAgIVAMcK/0VWQr2O3MNfJCydqR7oVELcMA0GCSqGSIb3DQEB
BQUAMIGQMUkwRwYDVQQDE0A1NjdjZGRmYzRjOWZiNTYwZTk1M2ZlZjA1N2M0NGFm
MDdiYjc4MDIzODIxYTA5NThiY2RmMGMwNzJhOTdiMThhMQswCQYDVQQGEwJVUzEN
MAsGA1UECBMEVXRhaDEQMA4GA1UEBxMHTWlkdmFsZTEVMBMGA1UEChMMU3BhY2Ug
TW9ua2V5MB4XDTEzMTIxNzE4MzgyMloXDTIzMTIxNTE4MzgyMlowgZAxSTBHBgNV
BAMTQDM4NTg3ODRkMjU1NTdiNTM1MWZmNjRmMmQzMTQ1ZjkwYTJlMTIzMDM4Y2Yz
Mjc1Yzg1OTM1MjcxYWIzMmNiMDkxCzAJBgNVBAYTAlVTMQ0wCwYDVQQIEwRVdGFo
MRAwDgYDVQQHEwdNaWR2YWxlMRUwEwYDVQQKEwxTcGFjZSBNb25rZXkwggEiMA0G
CSqGSIb3DQEBAQUAA4IBDwAwggEKAoIBAQDdf3icNvFsrlrnNLi8SocscqlSbFq+
pEvmhcSoqgDLqebnqu8Ld73HJJ74MGXEgRX8xZT5FinOML31CR6t9E/j3dqV6p+G
fdlFLe3IqtC0/bPVnCDBirBygBI4uCrMq+1VhAxPWclrDo7l9QRYbsExH9lfn+Ry
vxeNMZiOASasvVZNncY8E9usBGRdH17EfDL/TPwXqWOLyxSN5o54GTztjjy9w9CG
QP7jcCueKYyQJQCtEmnwc6P/q6/EPv5R6drBkX6loAPtmCUAkHqxkWOJrRq/v7Pw
zRYhfY+ZpVHGc7WEkDnLzRiUypr1C9oxvLKS10etZEIwEdKyOkSg2fdPAgMBAAGj
EzARMA8GA1UdEwEB/wQFMAMCAQAwDQYJKoZIhvcNAQEFBQADggEBAEcz0RTTJ99l
HTK/zTyfV5VZEhtwqu6bwre/hD7lhI+1ji0DZYGIgCbJLKuZhj+cHn2h5nPhN7zE
M9tc4pn0TgeVS0SVFSe6TGnIFipNogvP17E+vXpDZcW/xn9kPKeVCZc1hlDt1W4Z
5q+ub3aUwuMwYs7bcArtDrumCmciJ3LFyNhebPi4mntb5ooeLFLaujEmVYyrQnpo
tWKC9sMlJmLm4yAso64Sv9KLS2T9ivJBNn0ZtougozBCCTqrqgZVjha+B2yjHe9f
sRkg/uxcJf7wC5Y0BLlp1+aPwdmZD87T3a1uQ1Ij93jmHG+2T9U20MklHAePOl0q
yTqdSPnSH1c=
-----END CERTIFICATE-----
`)
	keyBytes = []byte(`-----BEGIN RSA PRIVATE KEY-----
MIIEpQIBAAKCAQEA3X94nDbxbK5a5zS4vEqHLHKpUmxavqRL5oXEqKoAy6nm56rv
C3e9xySe+DBlxIEV/MWU+RYpzjC99QkerfRP493aleqfhn3ZRS3tyKrQtP2z1Zwg
wYqwcoASOLgqzKvtVYQMT1nJaw6O5fUEWG7BMR/ZX5/kcr8XjTGYjgEmrL1WTZ3G
PBPbrARkXR9exHwy/0z8F6lji8sUjeaOeBk87Y48vcPQhkD+43ArnimMkCUArRJp
8HOj/6uvxD7+UenawZF+paAD7ZglAJB6sZFjia0av7+z8M0WIX2PmaVRxnO1hJA5
y80YlMqa9QvaMbyyktdHrWRCMBHSsjpEoNn3TwIDAQABAoIBAQCwgp6YzmgCFce3
LBpzYmjqEM3CMzr1ZXRe1gbr6d4Mbu7leyBX4SpJAnP0kIzo1X2yG7ol7XWPLOST
2pqqQWFQ00EX6wsJYEy+hmVRXl5HfU3MUkkAMwd9l3Xt4UWqKPBPD5XHvmN2fvl9
Y4388vXdseXGAGNK1eFs0TMjJuOtDxDyrmJcnxpJ7y/77y/Hb5rUa9DCvj8tkKHg
HmeIwQE0HhIFofj+qCYbqeVyjbPAaYZMrISXb2HmcyULKEOGRbMH24IzInKA0NxV
kdP9qmV8Y2bJ609Fft/y8Vpj31iEdq/OFXyobdVvnXMnaVyAetoaWy7AOTIQ2Cnw
wGbJ/F8BAoGBAN/pCnLQrWREeVMuFjf+MgYgCtRRaQ8EOVvjYcXXi0PhtOMFTAb7
djqhlgmBOFsmeXcb8YRZsF+pNtu1xk5RJOquyKfK8j1rUdAJfoxGHiaUFI2/1i9E
zuXX/Ao0xNRkWMxMKuwYBmmt1fMuVo+1M8UEwFMdHRtgxe+/+eOV1J2PAoGBAP09
7GLOYSYAI1OO3BN/bEVNau6tAxP5YShGmX2Qxy0+ooxHZ1V3D8yo6C0hSg+H+fPT
mjMgGcvaW6K+QyCdHDjgbk2hfdZ+Beq92JApPrH9gMV7MPhwHzgwjzDDio9OFxYY
3vjBQ2yX+9jvz9lkvq2NM3fqFqbsG6Et+5mCc6pBAoGBAI62bxVtEgbladrtdfXs
S6ABzkUzOl362EBL9iZuUnJKqstDtgiBQALwuLuIJA5cwHB9W/t6WuMt7CwveJy0
NW5rRrNDtBAXlgad9o2bp135ZfxO+EoadjCi8B7lMUsaRkq4hWcDjRrQVJxxvXRN
DxkVBSw0Uzf+/0nnN3OqLODbAoGACCY+/isAC1YDzQOS53m5RT2pjEa7C6CB1Ob4
t4a6MiWK25LMq35qXr6swg8JMBjDHWqY0r5ctievvTv8Mwd7SgVG526j+wwRKq2z
U2hQYS/0Peap+8S37Hn7kakpQ1VS/t4MBttJTSxS6XdGLAvG6xTZLCm3UuXUOcqe
ByGgkUECgYEAmop45kRi974g4MPvyLplcE4syb19ifrHj76gPRBi94Cp8jZosY1T
ucCCa4lOGgPtXJ0Qf1c8yq5vh4yqkQjrgUTkr+CFDGR6y4CxmNDQxEMYIajaIiSY
qmgvgyRayemfO2zR0CPgC6wSoGBth+xW6g+WA8y0z76ZSaWpFi8lVM4=
-----END RSA PRIVATE KEY-----
`)
)

func NetPipe(t testing.TB) (net.Conn, net.Conn) {
	l, err := net.Listen("tcp", "localhost:0")
	if err != nil {
		t.Fatal(err)
	}
	defer l.Close()
	client_future := utils.NewFuture()
	go func() {
		client_future.Set(net.Dial(l.Addr().Network(), l.Addr().String()))
	}()
	var errs utils.ErrorGroup
	server_conn, err := l.Accept()
	errs.Add(err)
	client_conn, err := client_future.Get()
	errs.Add(err)
	err = errs.Finalize()
	if err != nil {
		if server_conn != nil {
			server_conn.Close()
		}
		if client_conn != nil {
			client_conn.(net.Conn).Close()
		}
		t.Fatal(err)
	}
	return server_conn, client_conn.(net.Conn)
}

type HandshakingConn interface {
	net.Conn
	Handshake() error
}

func SimpleConnTest(t testing.TB, constructor func(
	t testing.TB, conn1, conn2 net.Conn) (sslconn1, sslconn2 HandshakingConn)) {
	server_conn, client_conn := NetPipe(t)
	defer server_conn.Close()
	defer client_conn.Close()

	data := "first test string\n"

	server, client := constructor(t, server_conn, client_conn)
	defer close_both(server, client)

	var wg sync.WaitGroup
	wg.Add(2)
	go func() {
		defer wg.Done()

		err := client.Handshake()
		if err != nil {
			t.Fatal(err)
		}

		_, err = io.Copy(client, bytes.NewReader([]byte(data)))
		if err != nil {
			t.Fatal(err)
		}

		err = client.Close()
		if err != nil {
			t.Fatal(err)
		}
	}()
	go func() {
		defer wg.Done()

		err := server.Handshake()
		if err != nil {
			t.Fatal(err)
		}

		buf := bytes.NewBuffer(make([]byte, 0, len(data)))
		_, err = io.CopyN(buf, server, int64(len(data)))
		if err != nil {
			t.Fatal(err)
		}
		if string(buf.Bytes()) != data {
			t.Fatal("mismatched data")
		}

		err = server.Close()
		if err != nil {
			t.Fatal(err)
		}
	}()
	wg.Wait()
}

func close_both(closer1, closer2 io.Closer) {
	var wg sync.WaitGroup
	wg.Add(2)
	go func() {
		defer wg.Done()
		closer1.Close()
	}()
	go func() {
		defer wg.Done()
		closer2.Close()
	}()
	wg.Wait()
}

func ClosingTest(t testing.TB, constructor func(
	t testing.TB, conn1, conn2 net.Conn) (sslconn1, sslconn2 HandshakingConn)) {

	run_test := func(close_tcp bool, server_writes bool) {
		server_conn, client_conn := NetPipe(t)
		defer server_conn.Close()
		defer client_conn.Close()
		server, client := constructor(t, server_conn, client_conn)
		defer close_both(server, client)

		var sslconn1, sslconn2 HandshakingConn
		var conn1 net.Conn
		if server_writes {
			sslconn1 = server
			conn1 = server_conn
			sslconn2 = client
		} else {
			sslconn1 = client
			conn1 = client_conn
			sslconn2 = server
		}

		var wg sync.WaitGroup
		wg.Add(2)
		go func() {
			defer wg.Done()
			_, err := sslconn1.Write([]byte("hello"))
			if err != nil {
				t.Fatal(err)
			}
			if close_tcp {
				err = conn1.Close()
			} else {
				err = sslconn1.Close()
			}
			if err != nil {
				t.Fatal(err)
			}
		}()

		go func() {
			defer wg.Done()
			data, err := ioutil.ReadAll(sslconn2)
			if err != nil {
				t.Fatal(err)
			}
			if !bytes.Equal(data, []byte("hello")) {
				t.Fatal("bytes don't match")
			}
		}()

		wg.Wait()
	}

	run_test(true, false)
	run_test(false, false)
	run_test(true, true)
	run_test(false, true)
}

func ThroughputBenchmark(b *testing.B, constructor func(
	t testing.TB, conn1, conn2 net.Conn) (sslconn1, sslconn2 HandshakingConn)) {
	server_conn, client_conn := NetPipe(b)
	defer server_conn.Close()
	defer client_conn.Close()

	server, client := constructor(b, server_conn, client_conn)
	defer close_both(server, client)

	b.SetBytes(1024)
	data := make([]byte, b.N*1024)
	_, err := io.ReadFull(rand.Reader, data[:])
	if err != nil {
		b.Fatal(err)
	}

	b.ResetTimer()
	var wg sync.WaitGroup
	wg.Add(2)
	go func() {
		defer wg.Done()
		_, err = io.Copy(client, bytes.NewReader([]byte(data)))
		if err != nil {
			b.Fatal(err)
		}
	}()
	go func() {
		defer wg.Done()

		buf := &bytes.Buffer{}
		_, err = io.CopyN(buf, server, int64(len(data)))
		if err != nil {
			b.Fatal(err)
		}
		if !bytes.Equal(buf.Bytes(), data) {
			b.Fatal("mismatched data")
		}
	}()
	wg.Wait()
	b.StopTimer()
}

func StdlibConstructor(t testing.TB, server_conn, client_conn net.Conn) (
	server, client HandshakingConn) {
	cert, err := tls.X509KeyPair(certBytes, keyBytes)
	if err != nil {
		t.Fatal(err)
	}
	config := &tls.Config{
		Certificates:       []tls.Certificate{cert},
		InsecureSkipVerify: true,
		CipherSuites:       []uint16{tls.TLS_RSA_WITH_AES_128_CBC_SHA}}
	server = tls.Server(server_conn, config)
	client = tls.Client(client_conn, config)
	return server, client
}

func passThruVerify(t testing.TB) func(bool, *CertificateStoreCtx) bool {
	x := func(ok bool, store *CertificateStoreCtx) bool {
		cert := store.GetCurrentCert()
		if cert == nil {
			t.Fatalf("Could not obtain cert from store\n")
		}
		sn := cert.GetSerialNumberHex()
		if len(sn) == 0 {
			t.Fatalf("Could not obtain serial number from cert")
		}
		return ok
	}
	return x
}

func OpenSSLConstructor(t testing.TB, server_conn, client_conn net.Conn) (
	server, client HandshakingConn) {
	ctx, err := NewCtx()
	if err != nil {
		t.Fatal(err)
	}
	ctx.SetVerify(VerifyNone, passThruVerify(t))
	key, err := LoadPrivateKeyFromPEM(keyBytes)
	if err != nil {
		t.Fatal(err)
	}
	err = ctx.UsePrivateKey(key)
	if err != nil {
		t.Fatal(err)
	}
	cert, err := LoadCertificateFromPEM(certBytes)
	if err != nil {
		t.Fatal(err)
	}
	err = ctx.UseCertificate(cert)
	if err != nil {
		t.Fatal(err)
	}
	err = ctx.SetCipherList("AES128-SHA")
	if err != nil {
		t.Fatal(err)
	}
	server, err = Server(server_conn, ctx)
	if err != nil {
		t.Fatal(err)
	}
	client, err = Client(client_conn, ctx)
	if err != nil {
		t.Fatal(err)
	}
	return server, client
}

func StdlibOpenSSLConstructor(t testing.TB, server_conn, client_conn net.Conn) (
	server, client HandshakingConn) {
	server_std, _ := StdlibConstructor(t, server_conn, client_conn)
	_, client_ssl := OpenSSLConstructor(t, server_conn, client_conn)
	return server_std, client_ssl
}

func OpenSSLStdlibConstructor(t testing.TB, server_conn, client_conn net.Conn) (
	server, client HandshakingConn) {
	_, client_std := StdlibConstructor(t, server_conn, client_conn)
	server_ssl, _ := OpenSSLConstructor(t, server_conn, client_conn)
	return server_ssl, client_std
}

func TestStdlibSimple(t *testing.T) {
	SimpleConnTest(t, StdlibConstructor)
}

func TestOpenSSLSimple(t *testing.T) {
	SimpleConnTest(t, OpenSSLConstructor)
}

func TestStdlibClosing(t *testing.T) {
	ClosingTest(t, StdlibConstructor)
}

func TestOpenSSLClosing(t *testing.T) {
	ClosingTest(t, OpenSSLConstructor)
}

func BenchmarkStdlibThroughput(b *testing.B) {
	ThroughputBenchmark(b, StdlibConstructor)
}

func BenchmarkOpenSSLThroughput(b *testing.B) {
	ThroughputBenchmark(b, OpenSSLConstructor)
}

func TestStdlibOpenSSLSimple(t *testing.T) {
	SimpleConnTest(t, StdlibOpenSSLConstructor)
}

func TestOpenSSLStdlibSimple(t *testing.T) {
	SimpleConnTest(t, OpenSSLStdlibConstructor)
}

func TestStdlibOpenSSLClosing(t *testing.T) {
	ClosingTest(t, StdlibOpenSSLConstructor)
}

func TestOpenSSLStdlibClosing(t *testing.T) {
	ClosingTest(t, OpenSSLStdlibConstructor)
}

func BenchmarkStdlibOpenSSLThroughput(b *testing.B) {
	ThroughputBenchmark(b, StdlibOpenSSLConstructor)
}

func BenchmarkOpenSSLStdlibThroughput(b *testing.B) {
	ThroughputBenchmark(b, OpenSSLStdlibConstructor)
}

func FullDuplexRenegotiationTest(t testing.TB, constructor func(
	t testing.TB, conn1, conn2 net.Conn) (sslconn1, sslconn2 HandshakingConn)) {

	server_conn, client_conn := NetPipe(t)
	defer server_conn.Close()
	defer client_conn.Close()

	times := 256
	data_len := 4 * SSLRecordSize
	data1 := make([]byte, data_len)
	_, err := io.ReadFull(rand.Reader, data1[:])
	if err != nil {
		t.Fatal(err)
	}
	data2 := make([]byte, data_len)
	_, err = io.ReadFull(rand.Reader, data1[:])
	if err != nil {
		t.Fatal(err)
	}

	server, client := constructor(t, server_conn, client_conn)
	defer close_both(server, client)

	var wg sync.WaitGroup

	send_func := func(sender HandshakingConn, data []byte) {
		defer wg.Done()
		for i := 0; i < times; i++ {
			if i == times/2 {
				wg.Add(1)
				go func() {
					defer wg.Done()
					err := sender.Handshake()
					if err != nil {
						t.Fatal(err)
					}
				}()
			}
			_, err := sender.Write(data)
			if err != nil {
				t.Fatal(err)
			}
		}
	}

	recv_func := func(receiver net.Conn, data []byte) {
		defer wg.Done()

		buf := make([]byte, len(data))
		for i := 0; i < times; i++ {
			n, err := io.ReadFull(receiver, buf[:])
			if err != nil {
				t.Fatal(err)
			}
			if !bytes.Equal(buf[:n], data) {
				t.Fatal(err)
			}
		}
	}

	wg.Add(4)
	go recv_func(server, data1)
	go send_func(client, data1)
	go send_func(server, data2)
	go recv_func(client, data2)
	wg.Wait()
}

func TestStdlibFullDuplexRenegotiation(t *testing.T) {
	FullDuplexRenegotiationTest(t, StdlibConstructor)
}

func TestOpenSSLFullDuplexRenegotiation(t *testing.T) {
	FullDuplexRenegotiationTest(t, OpenSSLConstructor)
}

func TestOpenSSLStdlibFullDuplexRenegotiation(t *testing.T) {
	FullDuplexRenegotiationTest(t, OpenSSLStdlibConstructor)
}

func TestStdlibOpenSSLFullDuplexRenegotiation(t *testing.T) {
	FullDuplexRenegotiationTest(t, StdlibOpenSSLConstructor)
}

func LotsOfConns(t *testing.T, payload_size int64, loops, clients int,
	sleep time.Duration, newListener func(net.Listener) net.Listener,
	newClient func(net.Conn) (net.Conn, error)) {
	tcp_listener, err := net.Listen("tcp", "localhost:0")
	if err != nil {
		t.Fatal(err)
	}
	ssl_listener := newListener(tcp_listener)
	go func() {
		for {
			conn, err := ssl_listener.Accept()
			if err != nil {
				t.Fatalf("failed accept: %s", err)
				continue
			}
			go func() {
				defer func() {
					err = conn.Close()
					if err != nil {
						t.Fatalf("failed closing: %s", err)
					}
				}()
				for i := 0; i < loops; i++ {
					_, err := io.Copy(ioutil.Discard,
						io.LimitReader(conn, payload_size))
					if err != nil {
						t.Fatalf("failed reading: %s", err)
						return
					}
					_, err = io.Copy(conn, io.LimitReader(rand.Reader,
						payload_size))
					if err != nil {
						t.Fatalf("failed writing: %s", err)
						return
					}
				}
				time.Sleep(sleep)
			}()
		}
	}()
	var wg sync.WaitGroup
	for i := 0; i < clients; i++ {
		tcp_client, err := net.Dial(tcp_listener.Addr().Network(),
			tcp_listener.Addr().String())
		if err != nil {
			t.Fatal(err)
		}
		ssl_client, err := newClient(tcp_client)
		if err != nil {
			t.Fatal(err)
		}
		wg.Add(1)
		go func(i int) {
			defer func() {
				err = ssl_client.Close()
				if err != nil {
					t.Fatalf("failed closing: %s", err)
				}
				wg.Done()
			}()
			for i := 0; i < loops; i++ {
				_, err := io.Copy(ssl_client, io.LimitReader(rand.Reader,
					payload_size))
				if err != nil {
					t.Fatalf("failed writing: %s", err)
					return
				}
				_, err = io.Copy(ioutil.Discard,
					io.LimitReader(ssl_client, payload_size))
				if err != nil {
					t.Fatalf("failed reading: %s", err)
					return
				}
			}
			time.Sleep(sleep)
		}(i)
	}
	wg.Wait()
}

func TestStdlibLotsOfConns(t *testing.T) {
	tls_cert, err := tls.X509KeyPair(certBytes, keyBytes)
	if err != nil {
		t.Fatal(err)
	}
	tls_config := &tls.Config{
		Certificates:       []tls.Certificate{tls_cert},
		InsecureSkipVerify: true,
		CipherSuites:       []uint16{tls.TLS_RSA_WITH_AES_128_CBC_SHA}}
	LotsOfConns(t, 1024*64, 10, 100, 0*time.Second,
		func(l net.Listener) net.Listener {
			return tls.NewListener(l, tls_config)
		}, func(c net.Conn) (net.Conn, error) {
			return tls.Client(c, tls_config), nil
		})
}

func TestOpenSSLLotsOfConns(t *testing.T) {
	ctx, err := NewCtx()
	if err != nil {
		t.Fatal(err)
	}
	key, err := LoadPrivateKeyFromPEM(keyBytes)
	if err != nil {
		t.Fatal(err)
	}
	err = ctx.UsePrivateKey(key)
	if err != nil {
		t.Fatal(err)
	}
	cert, err := LoadCertificateFromPEM(certBytes)
	if err != nil {
		t.Fatal(err)
	}
	err = ctx.UseCertificate(cert)
	if err != nil {
		t.Fatal(err)
	}
	err = ctx.SetCipherList("AES128-SHA")
	if err != nil {
		t.Fatal(err)
	}
	LotsOfConns(t, 1024*64, 10, 100, 0*time.Second,
		func(l net.Listener) net.Listener {
			return NewListener(l, ctx)
		}, func(c net.Conn) (net.Conn, error) {
			return Client(c, ctx)
		})
}

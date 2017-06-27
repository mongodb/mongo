package connstring

import (
	"fmt"
	"net"
	"net/url"
	"strconv"
	"strings"
	"time"
)

// Parse parses the provided uri and returns a URI object.
func ParseURIConnectionString(s string) (ConnString, error) {
	var p parser
	err := p.parse(s)
	if err != nil {
		err = fmt.Errorf("error parsing uri (%s): %s", s, err)
	}
	return p.ConnString, err
}

// ConnString represents a connection string to mongodb.
type ConnString struct {
	Original                string
	AppName                 string
	AuthMechanism           string
	AuthMechanismProperties map[string]string
	AuthSource              string
	Connect                 ConnectMode
	ConnectTimeout          time.Duration
	Database                string
	FSync                   bool
	HeartbeatInterval       time.Duration
	Hosts                   []string
	Journal                 bool
	KerberosService         string
	KerberosServiceHost     string
	MaxConnIdleTime         time.Duration
	MaxConnLifeTime         time.Duration
	MaxConnsPerHost         uint16
	MaxConnsPerHostSet      bool
	MaxIdleConnsPerHost     uint16
	MaxIdleConnsPerHostSet  bool
	Password                string
	PasswordSet             bool
	ReadPreference          string
	ReadPreferenceTagSets   []map[string]string
	ReplicaSet              string
	ServerSelectionTimeout  time.Duration
	SocketTimeout           time.Duration
	Username                string
	UseSSL                  bool
	W                       string
	WTimeout                time.Duration

	Options        map[string][]string
	UnknownOptions map[string][]string
}

func (u *ConnString) String() string {
	return u.Original
}

// ConnectMode informs the driver on how to connect
// to the server.
type ConnectMode uint8

// ConnectMode constants.
const (
	AutoConnect ConnectMode = iota
	SingleConnect
)

type parser struct {
	ConnString

	haveWTimeoutMS bool
}

func (p *parser) parse(original string) error {
	p.Original = original

	uri := original
	var err error
	// scheme
	if !strings.HasPrefix(uri, "mongodb://") {
		return fmt.Errorf("scheme must be \"mongodb\"")
	}

	// user info
	uri = uri[10:]

	if idx := strings.Index(uri, "@"); idx != -1 {
		userInfo := uri[:idx]
		uri = uri[idx+1:]

		username := userInfo
		var password string

		if idx := strings.Index(userInfo, ":"); idx != -1 {
			username = userInfo[:idx]
			password = userInfo[idx+1:]
		}

		p.Username, err = url.QueryUnescape(username)
		if err != nil {
			return fmt.Errorf("invalid username: %s", err)
		}
		if len(password) > 1 {
			if strings.Contains(password, ":") {
				return fmt.Errorf("unescaped colon in password")
			}
			p.Password, err = url.QueryUnescape(password)
			if err != nil {
				return fmt.Errorf("invalid password: %s", err)
			}
			p.PasswordSet = true
		}

	}

	// hosts
	hosts := uri
	if idx := strings.IndexAny(uri, "/?@"); idx != -1 {
		if uri[idx] == '@' {
			return fmt.Errorf("unescaped @ sign in user info")
		}
		if uri[idx] == '?' {
			return fmt.Errorf("must have a / before the query ?")
		}

		hosts = uri[:idx]
	}

	for _, host := range strings.Split(hosts, ",") {
		err = p.addHost(host)
		if err != nil {
			return fmt.Errorf("invalid host \"%s\": %s", host, err)
		}
	}

	if len(p.Hosts) == 0 {
		return fmt.Errorf("must have at least 1 host")
	}

	uri = uri[len(hosts):]

	if len(uri) == 0 {
		return nil
	}

	if uri[0] != '/' {
		return fmt.Errorf("must have a / separator between hosts and path")
	}

	uri = uri[1:]
	if len(uri) == 0 {
		return nil
	}

	database := uri
	if idx := strings.IndexAny(uri, "?"); idx != -1 {
		database = uri[:idx]
	}

	p.Database, err = url.QueryUnescape(database)
	if err != nil {
		return fmt.Errorf("invalid database \"%s\": %s", database, err)
	}

	uri = uri[len(database):]

	if len(uri) == 0 {
		return nil
	}

	if uri[0] != '?' {
		return fmt.Errorf("must have a ? separator between path and query")
	}

	uri = uri[1:]
	if len(uri) == 0 {
		return nil
	}

	for _, pair := range strings.FieldsFunc(uri, func(r rune) bool { return r == ';' || r == '&' }) {
		err = p.addOption(pair)
		if err != nil {
			return err
		}
	}

	return nil
}

func (p *parser) addHost(host string) error {
	if host == "" {
		return nil
	}
	host, err := url.QueryUnescape(host)
	if err != nil {
		return fmt.Errorf("invalid host \"%s\": %s", host, err)
	}

	_, port, err := net.SplitHostPort(host)
	// this is unfortunate that SplitHostPort actually requires
	// a port to exist.
	if err != nil {
		if addrError, ok := err.(*net.AddrError); !ok || addrError.Err != "missing port in address" {
			return err
		}
	}

	if port != "" {
		d, err := strconv.Atoi(port)
		if err != nil {
			return fmt.Errorf("port must be an integer: %s", err)
		}
		if d <= 0 || d >= 65536 {
			return fmt.Errorf("port must be in the range [1, 65535]")
		}
	}
	p.Hosts = append(p.Hosts, host)
	return nil
}

func (p *parser) addOption(pair string) error {
	kv := strings.SplitN(pair, "=", 2)
	if len(kv) != 2 || kv[0] == "" {
		return fmt.Errorf("invalid option")
	}

	key, err := url.QueryUnescape(kv[0])
	if err != nil {
		return fmt.Errorf("invalid option key \"%s\": %s", kv[0], err)
	}

	value, err := url.QueryUnescape(kv[1])
	if err != nil {
		return fmt.Errorf("invalid option value \"%s\": %s", kv[1], err)
	}

	lowerKey := strings.ToLower(key)
	switch lowerKey {
	case "appname":
		p.AppName = value
	case "authmechanism":
		p.AuthMechanism = value
	case "authmechanismproperties":
		p.AuthMechanismProperties = make(map[string]string)
		pairs := strings.Split(value, ",")
		for _, pair := range pairs {
			kv := strings.SplitN(pair, ":", 2)
			if len(kv) != 2 || kv[0] == "" {
				return fmt.Errorf("invalid authMechanism property")
			}
			p.AuthMechanismProperties[kv[0]] = kv[1]
		}
	case "authsource":
		p.AuthSource = value
	case "connect":
		switch strings.ToLower(value) {
		case "auto", "automatic":
			p.Connect = AutoConnect
		case "direct", "single":
			p.Connect = SingleConnect
		default:
			return fmt.Errorf("invalid 'connect' value: %s", value)
		}
	case "connecttimeoutms":
		n, err := strconv.Atoi(value)
		if err != nil || n < 0 {
			return fmt.Errorf("invalid value for %s: %s", key, value)
		}
		p.ConnectTimeout = time.Duration(n) * time.Millisecond
	case "heartbeatintervalms", "heartbeatfrequencyms":
		n, err := strconv.Atoi(value)
		if err != nil || n < 0 {
			return fmt.Errorf("invalid value for %s: %s", key, value)
		}
		p.HeartbeatInterval = time.Duration(n) * time.Millisecond
	case "fsync":
		f, err := strconv.ParseBool(value)
		if err != nil {
			return fmt.Errorf("invalid value for %s: %s", key, value)
		}
		p.FSync = f
	case "j":
		j, err := strconv.ParseBool(value)
		if err != nil {
			return fmt.Errorf("invalid value for %s: %s", key, value)
		}
		p.Journal = j
	case "gssapiservicename":
		p.KerberosService = value
	case "gssapihostname":
		p.KerberosServiceHost = value
	case "maxconnsperhost":
		n, err := strconv.Atoi(value)
		if err != nil || n < 0 {
			return fmt.Errorf("invalid value for %s: %s", key, value)
		}
		p.MaxConnsPerHost = uint16(n)
		p.MaxConnsPerHostSet = true
	case "maxidleconnsperhost":
		n, err := strconv.Atoi(value)
		if err != nil || n < 0 {
			return fmt.Errorf("invalid value for %s: %s", key, value)
		}
		p.MaxIdleConnsPerHost = uint16(n)
		p.MaxIdleConnsPerHostSet = true
	case "maxidletimems":
		n, err := strconv.Atoi(value)
		if err != nil || n < 0 {
			return fmt.Errorf("invalid value for %s: %s", key, value)
		}
		p.MaxConnIdleTime = time.Duration(n) * time.Millisecond
	case "maxlifetimems":
		n, err := strconv.Atoi(value)
		if err != nil || n < 0 {
			return fmt.Errorf("invalid value for %s: %s", key, value)
		}
		p.MaxConnLifeTime = time.Duration(n) * time.Millisecond
	case "maxpoolsize":
		n, err := strconv.Atoi(value)
		if err != nil || n < 0 {
			return fmt.Errorf("invalid value for %s: %s", key, value)
		}
		p.MaxConnsPerHost = uint16(n)
		p.MaxConnsPerHostSet = true
		p.MaxIdleConnsPerHost = uint16(n)
		p.MaxIdleConnsPerHostSet = true
	case "readpreference":
		p.ReadPreference = value
	case "readpreferencetags":
		tags := make(map[string]string)
		items := strings.Split(value, ",")
		for _, item := range items {
			parts := strings.Split(item, ":")
			if len(parts) != 2 {
				return fmt.Errorf("invalid value for %s: %s", key, value)
			}
			tags[parts[0]] = parts[1]
		}
		p.ReadPreferenceTagSets = append(p.ReadPreferenceTagSets, tags)
	case "replicaset":
		p.ReplicaSet = value
	case "serverselectiontimeoutms":
		n, err := strconv.Atoi(value)
		if err != nil || n < 0 {
			return fmt.Errorf("invalid value for %s: %s", key, value)
		}
		p.ServerSelectionTimeout = time.Duration(n) * time.Millisecond
	case "sockettimeoutms":
		n, err := strconv.Atoi(value)
		if err != nil || n < 0 {
			return fmt.Errorf("invalid value for %s: %s", key, value)
		}
		p.SocketTimeout = time.Duration(n) * time.Millisecond
	case "ssl":
		b, err := strconv.ParseBool(value)
		if err != nil {
			return fmt.Errorf("invalid value for %s: %s", key, value)
		}
		p.UseSSL = b
	case "w":
		p.W = value
	case "wtimeoutms":
		n, err := strconv.Atoi(value)
		if err != nil || n < 0 {
			return fmt.Errorf("invalid value for %s: %s", key, value)
		}
		p.WTimeout = time.Duration(n) * time.Millisecond
		p.haveWTimeoutMS = true
	case "wtimeout":
		if p.haveWTimeoutMS {
			// use wtimeoutMS if it exists
			break
		}
		n, err := strconv.Atoi(value)
		if err != nil || n < 0 {
			return fmt.Errorf("invalid value for %s: %s", key, value)
		}
		p.WTimeout = time.Duration(n) * time.Millisecond
	default:
		if p.UnknownOptions == nil {
			p.UnknownOptions = make(map[string][]string)
		}
		p.UnknownOptions[lowerKey] = append(p.UnknownOptions[lowerKey], value)
	}

	if p.Options == nil {
		p.Options = make(map[string][]string)
	}
	p.Options[lowerKey] = append(p.Options[lowerKey], value)

	return nil
}

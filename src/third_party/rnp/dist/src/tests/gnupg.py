import copy
from cli_common import run_proc

class GnuPG(object):
    def __init__(self, homedir, gpg_path):
        self.__gpg = gpg_path
        self.__common_params = ['--homedir', homedir, '--yes']
        self.__password = None
        self.__userid = None
        self.__hash = None

    @property
    def bin(self):
        return self.__gpg

    @property
    def common_params(self):
        return copy.copy(self.__common_params)

    @property
    def password(self):
        return self.__password

    @property
    def userid(self):
        return self.__userid

    @userid.setter
    def userid(self, val):
        self.__userid = val

    @property
    def hash(self):
        return self.__hash

    @hash.setter
    def hash(self, val):
        self.__hash = val

    @password.setter
    def password(self, val):
        self.__password = val

    def copy(self):
        return copy.deepcopy(self)

    def _run(self, cmd, params, batch_input = None):
        retcode, _, _ = run_proc(cmd, params, batch_input)
        return retcode == 0

    def list_keys(self, secret = False):
        params = ['--list-secret-keys'] if secret else ['--list-keys']
        params = params + self.common_params
        return self._run(self.__gpg, params)

    def generate_key_batch(self, batch_input):
        params = ['--gen-key', '--expert', '--batch',
                  '--pinentry-mode', 'loopback'] + self.common_params
        if self.password:
            params += ['--passphrase', self.password]
        if self.hash:
            params += ['--cert-digest-algo', self.hash]
        return self._run(self.__gpg, params, batch_input)

    def export_key(self, out_filename, secret=False):
        params = ['--armor']
        if secret:
            params += ['--pinentry-mode', 'loopback', '--export-secret-key']
            params += ['--passphrase', self.password]
        else:
           params = ['--export']

        params = self.common_params + \
            params + ['-o', out_filename, self.userid]
        return self._run(self.__gpg, params)

    def import_key(self, filename, secret = False):
        params = self.common_params
        if secret:
            params += ['--trust-model', 'always']
            params += ['--batch']
            params += ['--passphrase', self.password]
        params += ['--import', filename]
        return self._run(self.__gpg, params)

    def sign(self, out, input):
        params = self.common_params
        params += ['--passphrase', self.password]
        params += ['--batch']
        params += ['--pinentry-mode', 'loopback']
        params += ['-u', self.userid]
        params += ['-o', out]
        params += ['--sign', input]
        if self.hash:
            params += ['--digest-algo', self.hash]
        return self._run(self.__gpg, params)

    def verify(self, input):
        params = self.common_params
        params += ['--verify', input]
        if self.hash:
            params += ['--digest-algo', self.hash]
        return self._run(self.__gpg, params)

    def encrypt(self, recipient, out, input):
        params = self.common_params
        params += ['--passphrase', self.password]
        params += ['-r', recipient]
        params += ['-o', out]
        # Blindely trust the key without asking
        params += ['--always-trust']
        params += ['--encrypt', input]
        return self._run(self.__gpg, params)

    def decrypt(self, out, input):
        params = self.common_params
        params += ['--passphrase', self.password]
        params += ['--pinentry-mode', 'loopback']
        params += ['-o', out]
        params += ['--decrypt', input]
        return self._run(self.__gpg, params)

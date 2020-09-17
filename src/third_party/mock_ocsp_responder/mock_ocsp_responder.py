#
# This file has been modified in 2019 by MongoDB Inc.
#

# OCSPBuilder is derived from https://github.com/wbond/ocspbuilder
# OCSPResponder is derived from https://github.com/threema-ch/ocspresponder

# Copyright (c) 2015-2018 Will Bond <will@wbond.net>

# Permission is hereby granted, free of charge, to any person obtaining a copy of
# this software and associated documentation files (the "Software"), to deal in
# the Software without restriction, including without limitation the rights to
# use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
# of the Software, and to permit persons to whom the Software is furnished to do
# so, subject to the following conditions:

# The above copyright notice and this permission notice shall be included in all
# copies or substantial portions of the Software.

# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
# SOFTWARE.

# Copyright 2016 Threema GmbH

# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at

#    http://www.apache.org/licenses/LICENSE-2.0

# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

from __future__ import unicode_literals, division, absolute_import, print_function

import logging
import base64
import inspect
import re
import enum
import sys
import textwrap
from datetime import datetime, timezone, timedelta
from typing import Callable, Tuple, Optional

from asn1crypto import x509, keys, core, ocsp
from asn1crypto.ocsp import OCSPRequest, OCSPResponse
from oscrypto import asymmetric
from flask import Flask, request, Response

__version__ = '0.10.2'
__version_info__ = (0, 10, 2)

logger = logging.getLogger(__name__)

if sys.version_info < (3,):
    byte_cls = str
else:
    byte_cls = bytes

def _pretty_message(string, *params):
    """
    Takes a multi-line string and does the following:
     - dedents
     - converts newlines with text before and after into a single line
     - strips leading and trailing whitespace
    :param string:
        The string to format
    :param *params:
        Params to interpolate into the string
    :return:
        The formatted string
    """

    output = textwrap.dedent(string)

    # Unwrap lines, taking into account bulleted lists, ordered lists and
    # underlines consisting of = signs
    if output.find('\n') != -1:
        output = re.sub('(?<=\\S)\n(?=[^ \n\t\\d\\*\\-=])', ' ', output)

    if params:
        output = output % params

    output = output.strip()

    return output


def _type_name(value):
    """
    :param value:
        A value to get the object name of
    :return:
        A unicode string of the object name
    """

    if inspect.isclass(value):
        cls = value
    else:
        cls = value.__class__
    if cls.__module__ in set(['builtins', '__builtin__']):
        return cls.__name__
    return '%s.%s' % (cls.__module__, cls.__name__)

def _writer(func):
    """
    Decorator for a custom writer, but a default reader
    """

    name = func.__name__
    return property(fget=lambda self: getattr(self, '_%s' % name), fset=func)


class OCSPResponseBuilder(object):

    _response_status = None
    _certificate = None
    _certificate_status = None
    _revocation_date = None
    _certificate_issuer = None
    _hash_algo = None
    _key_hash_algo = None
    _nonce = None
    _this_update = None
    _next_update = None
    _response_data_extensions = None
    _single_response_extensions = None

    def __init__(self, response_status, certificate_status_list=[], revocation_date=None):
        """
        Unless changed, responses will use SHA-256 for the signature,
        and will be valid from the moment created for one week.
        :param response_status:
            A unicode string of OCSP response type:
            - "successful" - when the response includes information about the certificate
            - "malformed_request" - when the request could not be understood
            - "internal_error" - when an internal error occured with the OCSP responder
            - "try_later" - when the OCSP responder is temporarily unavailable
            - "sign_required" - when the OCSP request must be signed
            - "unauthorized" - when the responder is not the correct responder for the certificate
        :param certificate_list:
            A list of tuples with certificate serial number and certificate status objects.
            certificate_status:
                A unicode string of the status of the certificate. Only required if
                the response_status is "successful".
                - "good" - when the certificate is in good standing
                - "revoked" - when the certificate is revoked without a reason code
                - "key_compromise" - when a private key is compromised
                - "ca_compromise" - when the CA issuing the certificate is compromised
                - "affiliation_changed" - when the certificate subject name changed
                - "superseded" - when the certificate was replaced with a new one
                - "cessation_of_operation" - when the certificate is no longer needed
                - "certificate_hold" - when the certificate is temporarily invalid
                - "remove_from_crl" - only delta CRLs - when temporary hold is removed
                - "privilege_withdrawn" - one of the usages for a certificate was removed
                - "unknown" - the responder doesn't know about the certificate being requested
        :param revocation_date:
            A datetime.datetime object of when the certificate was revoked, if
            the response_status is "successful" and the certificate status is
            not "good" or "unknown".
        """
        self._response_status = response_status
        self._certificate_status_list = certificate_status_list
        self._revocation_date = revocation_date

        self._key_hash_algo = 'sha1'
        self._hash_algo = 'sha256'
        self._response_data_extensions = {}
        self._single_response_extensions = {}

    @_writer
    def nonce(self, value):
        """
        The nonce that was provided during the request.
        """

        if not isinstance(value, byte_cls):
            raise TypeError(_pretty_message(
                '''
                nonce must be a byte string, not %s
                ''',
                _type_name(value)
            ))

        self._nonce = value

    @_writer
    def certificate_issuer(self, value):
        """
        An asn1crypto.x509.Certificate object of the issuer of the certificate.
        This should only be set if the OCSP responder is not the issuer of
        the certificate, but instead a special certificate only for OCSP
        responses.
        """

        if value is not None:
            is_oscrypto = isinstance(value, asymmetric.Certificate)
            if not is_oscrypto and not isinstance(value, x509.Certificate):
                raise TypeError(_pretty_message(
                    '''
                    certificate_issuer must be an instance of
                    asn1crypto.x509.Certificate or
                    oscrypto.asymmetric.Certificate, not %s
                    ''',
                    _type_name(value)
                ))

            if is_oscrypto:
                value = value.asn1

        self._certificate_issuer = value

    @_writer
    def next_update(self, value):
        """
        A datetime.datetime object of when the response may next change. This
        should only be set if responses are cached. If responses are generated
        fresh on every request, this should not be set.
        """

        if not isinstance(value, datetime):
            raise TypeError(_pretty_message(
                '''
                next_update must be an instance of datetime.datetime, not %s
                ''',
                _type_name(value)
            ))

        self._next_update = value

    def build(self, responder_private_key=None, responder_certificate=None):
        """
        Validates the request information, constructs the ASN.1 structure and
        signs it.
        The responder_private_key and responder_certificate parameters are onlystr
        required if the response_status is "successful".
        :param responder_private_key:
            An asn1crypto.keys.PrivateKeyInfo or oscrypto.asymmetric.PrivateKey
            object for the private key to sign the response with
        :param responder_certificate:
            An asn1crypto.x509.Certificate or oscrypto.asymmetric.Certificate
            object of the certificate associated with the private key
        :return:
            An asn1crypto.ocsp.OCSPResponse object of the response
        """
        if self._response_status != 'successful':
            return ocsp.OCSPResponse({
                'response_status': self._response_status
            })

        is_oscrypto = isinstance(responder_private_key, asymmetric.PrivateKey)
        if not isinstance(responder_private_key, keys.PrivateKeyInfo) and not is_oscrypto:
            raise TypeError(_pretty_message(
                '''
                responder_private_key must be an instance ofthe c
                asn1crypto.keys.PrivateKeyInfo or
                oscrypto.asymmetric.PrivateKey, not %s
                ''',
                _type_name(responder_private_key)
            ))

        cert_is_oscrypto = isinstance(responder_certificate, asymmetric.Certificate)
        if not isinstance(responder_certificate, x509.Certificate) and not cert_is_oscrypto:
            raise TypeError(_pretty_message(
                '''
                responder_certificate must be an instance of
                asn1crypto.x509.Certificate or
                oscrypto.asymmetric.Certificate, not %s
                ''',
                _type_name(responder_certificate)
            ))

        if cert_is_oscrypto:
            responder_certificate = responder_certificate.asn1

        if self._certificate_status_list is None:
            raise ValueError(_pretty_message(
                '''
                certificate_status_list must be set if the response_status is
                "successful"
                '''
            ))

        def _make_extension(name, value):
            return {
                'extn_id': name,
                'critical': False,
                'extn_value': value
            }

        responses = []
        for serial, status in self._certificate_status_list:
            response_data_extensions = []
            single_response_extensions = []
            for name, value in self._response_data_extensions.items():
                response_data_extensions.append(_make_extension(name, value))
            if self._nonce:
                response_data_extensions.append(
                    _make_extension('nonce', self._nonce)
                )

            if not response_data_extensions:
                response_data_extensions = None

            for name, value in self._single_response_extensions.items():
                single_response_extensions.append(_make_extension(name, value))

            if self._certificate_issuer:
                single_response_extensions.append(
                    _make_extension(
                        'certificate_issuer',
                        [
                            x509.GeneralName(
                                name='directory_name',
                                value=self._certificate_issuer.subject
                            )
                        ]
                    )
                )

            if not single_response_extensions:
                single_response_extensions = None

            responder_key_hash = getattr(responder_certificate.public_key, self._key_hash_algo)

            if status == 'good':
                cert_status = ocsp.CertStatus(
                    name='good',
                    value=core.Null()
                )
            elif status == 'unknown':
                cert_status = ocsp.CertStatus(
                    name='unknown',
                    value=core.Null()
                )
            else:
                reason = status if status != 'revoked' else 'unspecified'
                cert_status = ocsp.CertStatus(
                    name='revoked',
                    value={
                        'revocation_time': self._revocation_date,
                        'revocation_reason': reason,
                    }
                )

            issuer = self._certificate_issuer if self._certificate_issuer else responder_certificate

            produced_at = datetime.now(timezone.utc).replace(microsecond=0)

            if self._this_update is None:
                self._this_update = produced_at

            if self._next_update is None:
                self._next_update = (self._this_update + timedelta(days=7)).replace(microsecond=0)

            response = {
                    'cert_id': {
                        'hash_algorithm': {
                            'algorithm': self._key_hash_algo
                        },
                        'issuer_name_hash': getattr(issuer.subject, self._key_hash_algo),
                        'issuer_key_hash': getattr(issuer.public_key, self._key_hash_algo),
                        'serial_number': serial,
                    },
                    'cert_status': cert_status,
                    'this_update': self._this_update,
                    'next_update': self._next_update,
                    'single_extensions': single_response_extensions
                }
            responses.append(response)

        response_data = ocsp.ResponseData({
            'responder_id': ocsp.ResponderId(name='by_key', value=responder_key_hash),
            'produced_at': produced_at,
            'responses': responses,
            'response_extensions': response_data_extensions
        })

        signature_algo = responder_private_key.algorithm
        if signature_algo == 'ec':
            signature_algo = 'ecdsa'

        signature_algorithm_id = '%s_%s' % (self._hash_algo, signature_algo)

        if responder_private_key.algorithm == 'rsa':
            sign_func = asymmetric.rsa_pkcs1v15_sign
        elif responder_private_key.algorithm == 'dsa':
            sign_func = asymmetric.dsa_sign
        elif responder_private_key.algorithm == 'ec':
            sign_func = asymmetric.ecdsa_sign

        if not is_oscrypto:
            responder_private_key = asymmetric.load_private_key(responder_private_key)
        signature_bytes = sign_func(responder_private_key, response_data.dump(), self._hash_algo)

        certs = None
        if self._certificate_issuer and getattr(self._certificate_issuer.public_key, self._key_hash_algo) != responder_key_hash:
            certs = [responder_certificate]

        return ocsp.OCSPResponse({
            'response_status': self._response_status,
            'response_bytes': {
                'response_type': 'basic_ocsp_response',
                'response': {
                    'tbs_response_data': response_data,
                    'signature_algorithm': {'algorithm': signature_algorithm_id},
                    'signature': signature_bytes,
                    'certs': certs,
                }
            }
        })

# Enums

class ResponseStatus(enum.Enum):
    successful = 'successful'
    malformed_request = 'malformed_request'
    internal_error = 'internal_error'
    try_later = 'try_later'
    sign_required = 'sign_required'
    unauthorized = 'unauthorized'


class CertificateStatus(enum.Enum):
    good = 'good'
    revoked = 'revoked'
    key_compromise = 'key_compromise'
    ca_compromise = 'ca_compromise'
    affiliation_changed = 'affiliation_changed'
    superseded = 'superseded'
    cessation_of_operation = 'cessation_of_operation'
    certificate_hold = 'certificate_hold'
    remove_from_crl = 'remove_from_crl'
    privilege_withdrawn = 'privilege_withdrawn'
    unknown = 'unknown'


# API endpoints
FAULT_REVOKED = "revoked"
FAULT_UNKNOWN = "unknown"

app = Flask(__name__)
class OCSPResponder:

    def __init__(self, issuer_cert: str, responder_cert: str, responder_key: str,
                       fault: str, next_update_seconds: int):
        """
        Create a new OCSPResponder instance.

        :param issuer_cert: Path to the issuer certificate.
        :param responder_cert: Path to the certificate of the OCSP responder
            with the `OCSP Signing` extension.
        :param responder_key: Path to the private key belonging to the
            responder cert.
        :param validate_func: A function that - given a certificate serial -
            will return the appropriate :class:`CertificateStatus` and -
            depending on the status - a revocation datetime.
        :param cert_retrieve_func: A function that - given a certificate serial -
            will return the corresponding certificate as a string.
        :param next_update_seconds: The ``nextUpdate`` value that will be written
            into the response. Default: 9 hours.

        """
        # Certs and keys
        self._issuer_cert = asymmetric.load_certificate(issuer_cert)
        self._responder_cert = asymmetric.load_certificate(responder_cert)
        self._responder_key = asymmetric.load_private_key(responder_key)

        # Next update
        self._next_update_seconds = next_update_seconds

        self._fault = fault

    def _fail(self, status: ResponseStatus) -> OCSPResponse:
        builder = OCSPResponseBuilder(response_status=status.value)
        return builder.build()

    def parse_ocsp_request(self, request_der: bytes) -> OCSPRequest:
        """
        Parse the request bytes, return an ``OCSPRequest`` instance.
        """
        return OCSPRequest.load(request_der)

    def validate(self):
        time = datetime(2018, 1, 1, 1, 00, 00, 00, timezone.utc)
        if self._fault == FAULT_REVOKED:
            return (CertificateStatus.revoked, time)
        elif self._fault == FAULT_UNKNOWN:
            return (CertificateStatus.unknown, None)
        elif self._fault != None:
            raise NotImplemented('Fault type could not be found')
        return (CertificateStatus.good, time)

    def _build_ocsp_response(self, ocsp_request: OCSPRequest) -> OCSPResponse:
        """
        Create and return an OCSP response from an OCSP request.
        """
        # Get the certificate serial
        tbs_request = ocsp_request['tbs_request']
        request_list = tbs_request['request_list']
        if len(request_list) < 1:
            logger.warning('Received OCSP request with no requests')
            raise NotImplemented('Empty requests not supported')

        single_request = request_list[0]  # TODO: Support more than one request
        req_cert = single_request['req_cert']
        serial = req_cert['serial_number'].native

        # Check certificate status
        try:
            certificate_status, revocation_date = self.validate()
        except Exception as e:
            logger.exception('Could not determine certificate status: %s', e)
            return self._fail(ResponseStatus.internal_error)

        certificate_status_list = [(serial, certificate_status.value)]

        # Build the response
        builder = OCSPResponseBuilder(**{
            'response_status': ResponseStatus.successful.value,
            'certificate_status_list': certificate_status_list,
            'revocation_date': revocation_date,
        })

        # Parse extensions
        for extension in tbs_request['request_extensions']:
            extn_id = extension['extn_id'].native
            critical = extension['critical'].native
            value = extension['extn_value'].parsed

            # This variable tracks whether any unknown extensions were encountered
            unknown = False

            # Handle nonce extension
            if extn_id == 'nonce':
                builder.nonce = value.native

            # That's all we know
            else:
                unknown = True

            # If an unknown critical extension is encountered (which should not
            # usually happen, according to RFC 6960 4.1.2), we should throw our
            # hands up in despair and run.
            if unknown is True and critical is True:
                logger.warning('Could not parse unknown critical extension: %r',
                        dict(extension.native))
                return self._fail(ResponseStatus.internal_error)

            # If it's an unknown non-critical extension, we can safely ignore it.
            elif unknown is True:
                logger.info('Ignored unknown non-critical extension: %r', dict(extension.native))

        # Set certificate issuer
        builder.certificate_issuer = self._issuer_cert

        # Set next update date
        now = datetime.now(timezone.utc)
        builder.next_update = (now + timedelta(seconds=self._next_update_seconds)).replace(microsecond=0)

        return builder.build(self._responder_key, self._responder_cert)

    def build_http_response(self, request_der: bytes) -> Response:
        global app
        response_der = self._build_ocsp_response(request_der).dump()
        resp = app.make_response((response_der, 200))
        resp.headers['content_type'] = 'application/ocsp-response'
        return resp


responder = None

def init_responder(issuer_cert: str, responder_cert: str, responder_key: str, fault: str, next_update_seconds: int):
    global responder
    responder = OCSPResponder(issuer_cert=issuer_cert, responder_cert=responder_cert, responder_key=responder_key, fault=fault, next_update_seconds=next_update_seconds)

def init(port=8080, debug=False, host=None):
    logger.info('Launching %sserver on port %d', 'debug' if debug else '', port)
    app.run(port=port, debug=debug, host=host)

@app.route('/', methods=['GET'])
def _handle_root():
    return 'ocsp-responder'

@app.route('/status/', defaults={'u_path': ''}, methods=['GET'])
@app.route('/status/<path:u_path>', methods=['GET'])
def _handle_get(u_path):
    global responder
    """
    An OCSP GET request contains the DER-in-base64 encoded OCSP request in the
    HTTP request URL.
    """
    if "Host" not in request.headers:
        raise ValueError ("Required 'Host' header not present")
    der = base64.b64decode(u_path)
    ocsp_request = responder.parse_ocsp_request(der)
    return responder.build_http_response(ocsp_request)

@app.route('/status', methods=['POST'])
def _handle_post():
    global responder
    """
    An OCSP POST request contains the DER encoded OCSP request in the HTTP
    request body.
    """
    if "Host" not in request.headers:
        raise ValueError ("Required 'Host' header not present")
    ocsp_request = responder.parse_ocsp_request(request.data)
    return responder.build_http_response(ocsp_request)

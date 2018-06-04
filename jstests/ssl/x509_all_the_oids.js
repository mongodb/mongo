// Test X509 auth with all known RDN OIDs.

(function() {
    'use strict';

    const SERVER_CERT = 'jstests/libs/server.pem';
    const CA_CERT = 'jstests/libs/ca.pem';

    function runTest(conn) {
        const script =
            'assert(db.getSiblingDB(\'$external\').auth({mechanism: \'MONGODB-X509\'}));';
        clearRawMongoProgramOutput();
        const exitCode = runMongoProgram('mongo',
                                         '--ssl',
                                         '--sslAllowInvalidHostnames',
                                         '--sslPEMKeyFile',
                                         'jstests/libs/client-all-the-oids.pem',
                                         '--sslCAFile',
                                         CA_CERT,
                                         '--port',
                                         conn.port,
                                         '--eval',
                                         script);

        // We expect failure, since we can't create a user with this massive username in WT.
        // But at least make sure the error message is sensible.
        assert.neq(exitCode, 0);
        const output = rawMongoProgramOutput();

        const NAME =
            'role=Datum-72,pseudonym=Datum-65,dmdName=Datum-54,deltaRevocationList=Datum-53,supportedAlgorithms=Datum-52,houseIdentifier=Datum-51,uniqueMember=Datum-50,distinguishedName=Datum-49,protocolInformation=Datum-48,enhancedSearchGuide=Datum-47,dnQualifier=Datum-46,x500UniqueIdentifier=Datum-45,generationQualifier=Datum-44,initials=Datum-43,GN=Datum-42,name=Datum-41,crossCertificatePair=Datum-40,certificateRevocationList=Datum-39,authorityRevocationList=Datum-38,cACertificate=Datum-37,userCertificate=Datum-36,userPassword=Datum-35,seeAlso=Datum-34,roleOccupant=Datum-33,owner=Datum-32,member=Datum-31,supportedApplicationContext=Datum-30,presentationAddress=Datum-29,preferredDeliveryMethod=Datum-28,destinationIndicator=Datum-27,registeredAddress=Datum-26,internationaliSDNNumber=Datum-25,x121Address=Datum-24,facsimileTelephoneNumber=Datum-23,teletexTerminalIdentifier=Datum-22,telexNumber=Datum-21,telephoneNumber=Datum-20,physicalDeliveryOfficeName=Datum-19,postOfficeBox=Datum-18,postalCode=Datum-17,postalAddress=Datum-16,businessCategory=Datum-15,searchGuide=Datum-14,description=Datum-13,title=Datum-12,OU=Datum-11,O=Datum-10,street=Datum-9,ST=NY,L=Datum-7,C=US,serialNumber=Datum-5,SN=Datum-4,CN=Datum-3';

        assert(output.includes('Error: Could not find user ' + NAME + '@$external'),
               "Shell is missing unknown user message");
    }

    // Standalone.
    const mongod = MongoRunner.runMongod({
        auth: '',
        sslMode: 'requireSSL',
        sslPEMKeyFile: SERVER_CERT,
        sslCAFile: CA_CERT,
        sslAllowInvalidCertificates: '',
    });
    runTest(mongod);
    MongoRunner.stopMongod(mongod);
})();

# EngFlow Certification Installation

MongoDB uses EngFlow to enable remote execution with Bazel. This dramatically speeds up the build process, but is only available to internal MongoDB employees.

To install the necessary credentials to enable remote execution, run scons.py with any build command, then follow the setup instructions it prints out. Or:

(Only if not in the Engineering org)

-   Request access to the MANA group https://mana.corp.mongodbgov.com/resources/659ec4b9bccf3819e5608712

(For everyone)

-   Go to https://sodalite.cluster.engflow.com/gettingstarted
-   Login with OKTA, then click the "GENERATE AND DOWNLOAD MTLS CERTIFICATE" button
    -   (If logging in with OKTA doesn't work) Login with Google using your MongoDB email, then click the "GENERATE AND DOWNLOAD MTLS CERTIFICATE" button
-   On your local system (usually your MacBook), open a shell terminal and, after setting the variables on the first three lines, run:

          REMOTE_USER=<SSH User from https://spruce.mongodb.com/spawn/host>
          REMOTE_HOST=<DNS Name from https://spruce.mongodb.com/spawn/host>
          ZIP_FILE=~/Downloads/engflow-mTLS.zip

          curl https://raw.githubusercontent.com/mongodb/mongo/master/buildscripts/setup_engflow_creds.sh -o setup_engflow_creds.sh
          chmod +x ./setup_engflow_creds.sh
          ./setup_engflow_creds.sh $REMOTE_USER $REMOTE_HOST $ZIP_FILE

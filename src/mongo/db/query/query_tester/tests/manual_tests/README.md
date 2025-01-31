# QueryTester manual tests

Every directory in here will be considered a test case by resmoke and evergreen.

Each directory here should have the following:

- A set of `.test` files that conform to the format specified [here](../../README.md)
- A corresponding set of `.results` files that contain the expected output.
- The required `.coll` files that conform to the format specified [here](../../README.md)

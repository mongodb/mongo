#! /usr/bin/env python3
"""
Simulates a human authenticating to azure on the Web, specifically with the
device authorization grant flow.

Given a device authorization endpoint, a username, a user code and a file with necessary setup information, it
will simulate automatically logging in as a human would.

"""

import argparse
import json
import traceback
from pathlib import Path

import geckodriver_autoinstaller
from selenium import webdriver
from selenium.common.exceptions import TimeoutException
from selenium.webdriver.common.by import By
from selenium.webdriver.firefox.options import Options
from selenium.webdriver.support import expected_conditions as EC
from selenium.webdriver.support.ui import WebDriverWait


def authenticate_azure(activation_endpoint, userCode, username, test_credentials):
    # Install GeckoDriver if needed.
    geckodriver_autoinstaller.install()

    # Launch headless Firefox to the device authorization endpoint.
    firefox_options = Options()
    firefox_options.add_argument("-headless")
    driver = webdriver.Firefox(options=firefox_options)
    driver.get(activation_endpoint)

    try:
        # User code will be added to the input box.
        # Wait for the user code prompt and next button to load.
        user_code_input_box = WebDriverWait(driver, 30).until(
            EC.presence_of_element_located((By.XPATH, "//input[@name='otc']"))
        )
        next_button = WebDriverWait(driver, 30).until(
            EC.presence_of_element_located((By.XPATH, "//input[@type='submit'][@value='Next']"))
        )

        # Enter usercode.
        user_code_input_box.send_keys(userCode)
        next_button.click()

        # Wait for the username prompt and next button to load.
        username_input_box = WebDriverWait(driver, 30).until(
            EC.presence_of_element_located((By.XPATH, "//input[@name='loginfmt']"))
        )
        next_button = WebDriverWait(driver, 30).until(
            EC.presence_of_element_located((By.XPATH, "//input[@type='submit'][@value='Next']"))
        )

        # Enter username.
        username_input_box.send_keys(username)
        next_button.click()

        # Click on "Use your password" button if it exists.
        try:
            use_password_button = WebDriverWait(driver, 30).until(
                EC.presence_of_element_located(
                    (By.XPATH, "//span[@role='button'][. = 'Use your password']")
                )
            )
            use_password_button.click()
        except TimeoutException:
            # No "use your password" button, the password input should already be on-screen.
            pass

        # Azure delivers two different HTML's so we try with both versions.
        password_input_box = None
        verify_button = None
        try:
            password_input_box = WebDriverWait(driver, 30).until(
                EC.presence_of_element_located((By.ID, "passwordEntry"))
            )
        except:
            password_input_box = WebDriverWait(driver, 30).until(
                EC.presence_of_element_located((By.ID, "i0118"))
            )

        try:
            verify_button = WebDriverWait(driver, 30).until(
                EC.presence_of_element_located((By.XPATH, "//button[@data-testid='primaryButton']"))
            )
        except:
            verify_button = WebDriverWait(driver, 30).until(
                EC.presence_of_element_located((By.ID, "idSIButton9"))
            )

        # Enter password.
        password_input_box.send_keys(test_credentials[username])
        verify_button.click()

        # Assert 'Are you trying to sign in to OIDC_EVG_TESTING?' message.
        continue_button = WebDriverWait(driver, 30).until(
            EC.presence_of_element_located((By.XPATH, "//input[@type='submit'][@value='Continue']"))
        )
        continue_button.click()

        # Assert that the landing page contains the "You have signed in to the OIDC_EVG_TESTING application on your device" text, indicating successful auth.
        landing_header = WebDriverWait(driver, 30).until(
            EC.presence_of_element_located(
                (By.XPATH, "//p[@id='message'][@class='text-block-body no-margin-top']")
            )
        )

        assert landing_header is not None and "You have signed in" in landing_header.text

    except Exception as e:
        print("Error: ", e)
        print("Traceback: ", traceback.format_exc())
        print("HTML Source: ", driver.page_source)
        raise
    else:
        print("Success")
    finally:
        driver.quit()


def main():
    parser = argparse.ArgumentParser(description="Azure Automated Authentication Simulator")

    parser.add_argument(
        "-e", "--activationEndpoint", type=str, help="Endpoint to start activation at"
    )
    parser.add_argument(
        "-c", "--userCode", type=str, help="Code to be added in the endpoint to authenticate"
    )
    parser.add_argument("-u", "--username", type=str, help="Username to authenticate as")
    parser.add_argument(
        "-s",
        "--setupFile",
        type=str,
        help="File containing information generated during test setup, relative to home directory",
    )

    args = parser.parse_args()

    with open(Path.home() / args.setupFile) as setup_file:
        setup_information = json.load(setup_file)
        assert args.username in setup_information
        assert setup_information[args.username]

        num_retries = 3
        success = False

        for i in range(num_retries):
            try:
                authenticate_azure(
                    args.activationEndpoint, args.userCode, args.username, setup_information
                )
                success = True
                break
            except Exception as e:
                print(f"Error authenticating (attempt {i+1}/{num_retries}): {e}")
                if i < num_retries - 1:
                    print("Retrying...")

        if success:
            print("Authentication with Azure was successful")
        else:
            print(f"Authentication with Azure failed after {num_retries} attempts")


if __name__ == "__main__":
    main()

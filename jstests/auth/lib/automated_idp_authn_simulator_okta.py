#! /usr/bin/env python3
"""
Simulates a human authenticating to an identity provider on the Web, specifically with the
device authorization grant flow.

Given a device authorization endpoint, a username, and a file with necessary setup information, it
will simulate automatically logging in as a human would.

"""

import argparse
import json
import traceback
from pathlib import Path

import geckodriver_autoinstaller
from selenium import webdriver
from selenium.webdriver.common.by import By
from selenium.webdriver.firefox.options import Options
from selenium.webdriver.support import expected_conditions as EC
from selenium.webdriver.support.ui import WebDriverWait


def get_input_box_with_label(driver, label_to_match, timeout):
    caps = "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
    label_xpath = (
        f"//label[contains(translate(., '{caps}', '{caps.lower()}'), '{label_to_match.lower()}')]"
    )
    label = WebDriverWait(driver, timeout).until(
        EC.presence_of_element_located((By.XPATH, label_xpath))
    )
    for_value = label.get_attribute("for")
    if not for_value:
        raise Exception(f"Failed to find input box with label '{label_to_match}'")
    return WebDriverWait(driver, timeout).until(
        EC.element_to_be_clickable((By.XPATH, f"//input[@id='{for_value}']"))
    )


def authenticate_okta(activation_endpoint, userCode, username, test_credentials):
    # Install GeckoDriver if needed.
    geckodriver_autoinstaller.install()

    # Launch headless Firefox to the device authorization endpoint.
    firefox_options = Options()
    firefox_options.add_argument("-headless")
    driver = webdriver.Firefox(options=firefox_options)
    driver.get(activation_endpoint)

    try:
        # Wait for activation code input box and next button to load and click.
        activationCode_input_box = get_input_box_with_label(driver, "Activation Code", 30)
        next_button = WebDriverWait(driver, 30).until(
            EC.element_to_be_clickable(
                (By.XPATH, "//input[@class='button button-primary'][@value='Next']")
            )
        )

        # Enter user activation code.
        activationCode_input_box.send_keys(userCode)
        next_button.click()

        # Wait for the username prompt and next button to load.
        username_input_box = get_input_box_with_label(driver, "Username", 30)
        next_button_username = WebDriverWait(driver, 30).until(
            EC.element_to_be_clickable(
                (By.XPATH, "//input[@class='button button-primary'][@value='Next']")
            )
        )

        # Enter username.
        username_input_box.send_keys(username)
        next_button_username.click()

        # Wait for the password prompt and next button to load.
        password_input_box = get_input_box_with_label(driver, "Password", 30)
        verify_button = WebDriverWait(driver, 30).until(
            EC.element_to_be_clickable(
                (By.XPATH, "//input[@class='button button-primary'][@value='Verify']")
            )
        )

        # Enter password.
        password_input_box.send_keys(test_credentials[username])
        verify_button.click()

        # Assert that the landing page contains the "Device activated" text, indicating successful auth.
        landing_header = WebDriverWait(driver, 30).until(
            EC.presence_of_element_located(
                (
                    By.XPATH,
                    "//h2[@class='okta-form-title o-form-head'][contains(text(), 'Device activated')]",
                )
            )
        )
        assert landing_header is not None

    except Exception as e:
        print("Error: ", e)
        print("Traceback: ", traceback.format_exc())
        print("HTML Source: ", driver.page_source)
    else:
        print("Success")
    finally:
        driver.quit()


def main():
    parser = argparse.ArgumentParser(description="Okta Automated Authentication Simulator")

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

        authenticate_okta(args.activationEndpoint, args.userCode, args.username, setup_information)


if __name__ == "__main__":
    main()

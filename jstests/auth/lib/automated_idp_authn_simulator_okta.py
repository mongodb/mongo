#! /usr/bin/env python3
"""
Simulates a human authenticating to an identity provider on the Web, specifically with the
device authorization grant flow.

Given a device authorization endpoint, a username, and a file with necessary setup information, it
will simulate automatically logging in as a human would.

"""
import argparse
import os
import json
import traceback

import geckodriver_autoinstaller
from pathlib import Path
from selenium import webdriver
from selenium.webdriver.common.by import By
from selenium.webdriver.firefox.options import Options
from selenium.webdriver.support import expected_conditions as EC
from selenium.webdriver.support.ui import WebDriverWait

def authenticate_okta(activation_endpoint, userCode, username, test_credentials):
    # Install GeckoDriver if needed.
    geckodriver_autoinstaller.install()

    # Launch headless Firefox to the device authorization endpoint.
    firefox_options = Options()
    firefox_options.add_argument('-headless')
    driver = webdriver.Firefox(options=firefox_options)
    driver.get(activation_endpoint)

    try:
        # Wait for activation code input box and next button to load and click.
        activationCode_input_box = WebDriverWait(driver, 30).until(
            EC.presence_of_element_located((By.XPATH, "//input[@id='user-code']"))
        )
        next_button = WebDriverWait(driver, 30).until(
            EC.presence_of_element_located((By.XPATH, "//input[@class='button button-primary'][@value='Next']"))
        )
        
        # Enter user activation code.
        activationCode_input_box.send_keys(userCode)
        next_button.click()

        # Wait for the username prompt and next button to load.
        username_input_box = WebDriverWait(driver, 30).until(
            EC.presence_of_element_located((By.XPATH, "//input[@name='username']"))
        )
        next_button_username = WebDriverWait(driver, 30).until(
            EC.presence_of_element_located((By.XPATH, "//input[@id='idp-discovery-submit'][@value='Next']"))
        )
        
        # Enter username.
        username_input_box.send_keys(username)
        next_button_username.click()

        # Wait for the password prompt and next button to load.
        password_input_box = WebDriverWait(driver, 30).until(
            EC.presence_of_element_located((By.XPATH, "//input[@name='password']"))
        )
        verify_button = WebDriverWait(driver, 30).until(
            EC.presence_of_element_located((By.XPATH, "//input[@class='button button-primary'][@value='Sign In']"))
        )

        # Enter password.
        password_input_box.send_keys(test_credentials[username])
        verify_button.click()

        # Assert that the landing page contains the "Device activated" text, indicating successful auth.
        landing_header = WebDriverWait(driver, 30).until(
            EC.presence_of_element_located((By.XPATH, "//h2[@class='okta-form-title o-form-head'][contains(text(), 'Device activated')]"))
        )
        assert landing_header is not None
        
    except Exception as e:
        print("Error: ", e)
        print("Traceback: ", traceback.format_exc())
        print("HTML Source: ", driver.page_source)
    else:
        print('Success')
    finally:
        driver.quit()

def main():
    parser = argparse.ArgumentParser(description='Okta Automated Authentication Simulator')

    parser.add_argument('-e', '--activationEndpoint', type=str, help="Endpoint to start activation at")
    parser.add_argument('-c', '--userCode', type=str, help="Code to be added in the endpoint to authenticate")
    parser.add_argument('-u', '--username', type=str, help="Username to authenticate as")
    parser.add_argument('-s', '--setupFile', type=str, help="File containing information generated during test setup, relative to home directory")

    args = parser.parse_args()

    with open(Path.home() / args.setupFile) as setup_file:
        setup_information = json.load(setup_file)
        assert args.username in setup_information
        assert setup_information[args.username]

        authenticate_okta(args.activationEndpoint, args.userCode, args.username, setup_information)

if __name__ == '__main__':
    main()

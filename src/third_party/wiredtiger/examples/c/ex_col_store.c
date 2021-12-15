/*-
 * Public Domain 2014-present MongoDB, Inc.
 * Public Domain 2008-2014 WiredTiger, Inc.
 *
 * This is free and unencumbered software released into the public domain.
 *
 * Anyone is free to copy, modify, publish, use, compile, sell, or
 * distribute this software, either in source code form or as a compiled
 * binary, for any purpose, commercial or non-commercial, and by any
 * means.
 *
 * In jurisdictions that recognize copyright laws, the author or authors
 * of this software dedicate any and all copyright interest in the
 * software to the public domain. We make this dedication for the benefit
 * of the public at large and to the detriment of our heirs and
 * successors. We intend this dedication to be an overt act of
 * relinquishment in perpetuity of all present and future rights to this
 * software under copyright law.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 *
 * ex_col_store.c
 *	This is an example application that demonstrates column-store operations.
 */

#include <test_util.h>
#include <assert.h>

#define NUM_ENTRIES 100
#define TABLE_NAME "table:weather"
#define NUM_REC 5
#define NUM_COUNTRIES 7

static const char *home;

/*! [col-store decl] */
typedef struct {
    uint16_t hour;
    uint16_t pressure;
    uint16_t loc_lat;
    uint16_t loc_long;
    uint8_t temp;
    uint8_t humidity;
    uint8_t wind;
    uint8_t feels_like_temp;
    char day[5];
    char country[5];
} WEATHER;

/*! [col-store decl] */

static void update_celsius_to_fahrenheit(WT_SESSION *session);
static void print_all_columns(WT_SESSION *session);
static void generate_data(WEATHER *w_array);
static void remove_country(WT_SESSION *session);
static void average_data(WT_SESSION *session, char *country_average);
static int find_min_and_max_temp(
  WT_SESSION *session, uint16_t start_time, uint16_t end_time, int *min_temp, int *max_temp);

static void
print_all_columns(WT_SESSION *session)
{
    WT_CURSOR *cursor;
    WT_DECL_RET;
    uint64_t recno;
    uint16_t hour, loc_lat, loc_long, pressure;
    uint8_t feels_like_temp, humidity, temp, wind;
    const char *country, *day;

    error_check(session->open_cursor(session, TABLE_NAME, NULL, NULL, &cursor));
    while ((ret = cursor->next(cursor)) == 0) {
        error_check(cursor->get_key(cursor, &recno));
        error_check(cursor->get_value(cursor, &hour, &pressure, &loc_lat, &loc_long, &temp,
          &humidity, &wind, &feels_like_temp, &day, &country));

        printf(
          "{\n"
          "    ID: %" PRIu64
          "\n"
          "    day: %s\n"
          "    hour: %" PRIu16
          "\n"
          "    temp: %" PRIu8
          "\n"
          "    humidity: %" PRIu8
          "\n"
          "    pressure: %" PRIu16
          "\n"
          "    wind: %" PRIu8
          "\n"
          "    feels like: %" PRIu8
          "\n"
          "    lat: %" PRIu16
          "\n"
          "    long: %" PRIu16
          "\n"
          "    country: %s\n"
          "}\n\n",
          recno, day, hour, temp, humidity, pressure, wind, feels_like_temp, loc_lat, loc_long,
          country);
    }
    scan_end_check(ret == WT_NOTFOUND);
    error_check(cursor->close(cursor));
}

static void
update_celsius_to_fahrenheit(WT_SESSION *session)
{
    WT_CURSOR *cursor;
    WT_DECL_RET;
    uint8_t temp, temp_in_fahrenheit;

    printf("Converting temperature from celsius to fahrenheit.\n");

    /*! [col-store temperature] */
    error_check(session->open_cursor(session, "colgroup:weather:temperature", NULL, NULL, &cursor));
    while ((ret = cursor->next(cursor)) == 0) {
        error_check(cursor->get_value(cursor, &temp));

        /*
         * Update the value from celsius to fahrenheit. Discarding decimals and keeping data simple
         * by type casting to uint8_t.
         */
        temp_in_fahrenheit = (uint8_t)((1.8 * temp) + 32.0);

        cursor->set_value(cursor, temp_in_fahrenheit);
        error_check(cursor->update(cursor));
    }
    scan_end_check(ret == WT_NOTFOUND);
    error_check(cursor->close(cursor));
    /*! [col-store temperature] */
}

static void
remove_country(WT_SESSION *session)
{
    WT_CURSOR *cursor;
    WT_DECL_RET;
    uint64_t recno;
    uint16_t loc_lat, loc_long;
    const char *country;

    printf("Removing all data for country AUS.\n");
    error_check(session->open_cursor(session, "colgroup:weather:location", NULL, NULL, &cursor));
    /*
     * All Australian data is being removed, to test if deletion works.
     */
    while ((ret = cursor->next(cursor)) == 0) {
        error_check(cursor->get_key(cursor, &recno));
        error_check(cursor->get_value(cursor, &loc_lat, &loc_long, &country));
        if (strcmp("AUS", country) == 0) {
            error_check(cursor->remove(cursor));
        }
    }
    scan_end_check(ret == WT_NOTFOUND);
    error_check(cursor->close(cursor));
}

static void
generate_data(WEATHER *w_array)
{
    WEATHER w;
    int country, day;

    srand((unsigned int)getpid());

    for (int i = 0; i < NUM_ENTRIES; i++) {
        day = rand() % 7;
        switch (day) {
        case 0:
            strcpy(w.day, "MON");
            break;
        case 1:
            strcpy(w.day, "TUE");
            break;
        case 2:
            strcpy(w.day, "WED");
            break;
        case 3:
            strcpy(w.day, "THU");
            break;
        case 4:
            strcpy(w.day, "FRI");
            break;
        case 5:
            strcpy(w.day, "SAT");
            break;
        case 6:
            strcpy(w.day, "SUN");
            break;
        default:
            assert(false);
        }
        /* 24-hour-time 0-2400. */
        w.hour = (uint16_t)(rand() % 2401);
        /* Temperature range: 0-50C.  */
        w.temp = (uint8_t)(rand() % 51);
        /* Feels like temperature range 0-50C */
        w.feels_like_temp = (uint8_t)(rand() % 51);
        /* Humidity range: 0-100%. */
        w.humidity = (uint8_t)(rand() % 101);
        /* Pressure range: 900-1100pa */
        w.pressure = (uint16_t)((rand() % (1100 + 1 - 900)) + 900);
        /* Wind range: 0-200 km/hr. */
        w.wind = (uint8_t)(rand() % 201);
        /* latitude: 0-180 degrees. */
        w.loc_lat = (uint16_t)(rand() % 181);
        /* longitude: 0-90 degrees. */
        w.loc_long = (uint16_t)(rand() % 91);

        country = rand() % 7;
        switch (country) {
        case 0:
            strcpy(w.country, "AUS");
            break;
        case 1:
            strcpy(w.country, "GBR");
            break;
        case 2:
            strcpy(w.country, "USA");
            break;
        case 3:
            strcpy(w.country, "NZD");
            break;
        case 4:
            strcpy(w.country, "IND");
            break;
        case 5:
            strcpy(w.country, "CHI");
            break;
        case 6:
            strcpy(w.country, "RUS");
            break;
        default:
            assert(false);
        }

        w_array[i] = w;
    }
}

/*
 * find_min_and_max_temp --
 *     The function returns 0 when a valid min/max temperature can be calculated given the time
 *     range. If no records are found it will return WT_NOTFOUND, otherwise the program will crash
 *     if an internal error is encountered.
 */
static int
find_min_and_max_temp(
  WT_SESSION *session, uint16_t start_time, uint16_t end_time, int *min_temp, int *max_temp)
{
    WT_CURSOR *end_time_cursor, *join_cursor, *start_time_cursor;
    WT_DECL_RET;
    uint64_t recno;
    int exact;
    uint16_t hour;
    uint8_t temp;

    /*! [col-store join] */

    /* Open cursors needed by the join. */
    error_check(
      session->open_cursor(session, "join:table:weather(hour,temp)", NULL, NULL, &join_cursor));
    error_check(
      session->open_cursor(session, "index:weather:hour", NULL, NULL, &start_time_cursor));
    error_check(session->open_cursor(session, "index:weather:hour", NULL, NULL, &end_time_cursor));

    /*
     * Select values WHERE (hour >= start AND hour <= end). Find the starting record closest to
     * desired start time.
     */
    start_time_cursor->set_key(start_time_cursor, start_time);
    error_check(start_time_cursor->search_near(start_time_cursor, &exact));
    if (exact == -1) {
        ret = start_time_cursor->next(start_time_cursor);
        if (ret == WT_NOTFOUND)
            return ret;
        else
            error_check(ret);
    }

    error_check(session->join(session, join_cursor, start_time_cursor, "compare=ge"));

    /* Find the ending record closest to desired end time. */
    end_time_cursor->set_key(end_time_cursor, end_time);
    error_check(end_time_cursor->search_near(end_time_cursor, &exact));
    if (exact == 1) {
        ret = end_time_cursor->prev(end_time_cursor);
        if (ret == WT_NOTFOUND)
            return ret;
        else
            error_check(ret);
    }

    error_check(session->join(session, join_cursor, end_time_cursor, "compare=le"));

    /* Initialize minimum temperature and maximum temperature to temperature of the first record. */
    ret = join_cursor->next(join_cursor);
    if (ret == WT_NOTFOUND)
        return ret;
    else
        error_check(ret);

    error_check(join_cursor->get_key(join_cursor, &recno));
    error_check(join_cursor->get_value(join_cursor, &hour, &temp));
    *min_temp = temp;
    *max_temp = temp;

    /* Iterating through found records between start and end time to find the min & max temps. */
    while ((ret = join_cursor->next(join_cursor)) == 0) {
        error_check(join_cursor->get_value(join_cursor, &hour, &temp));

        *min_temp = WT_MIN(*min_temp, temp);
        *max_temp = WT_MAX(*max_temp, temp);
    }

    /*! [col-store join] */

    /*
     * If WT_NOTFOUND is hit at this point, it is because we have traversed through all temperature
     * records, hence we return 0 to the calling function to signal success. Otherwise an internal
     * error was hit.
     */
    if (ret != WT_NOTFOUND)
        error_check(ret);

    return (0);
}

/*
 * average_data --
 *     Obtains the average data across all fields given a specific location.
 */
void
average_data(WT_SESSION *session, char *country_average)
{
    WT_CURSOR *loc_cursor;
    WT_DECL_RET;
    unsigned int count;
    /* rec_arr holds the sum of the records in order to obtain the averages. */
    unsigned int rec_arr[NUM_REC];
    uint16_t hour, loc_lat, loc_long, pressure;
    uint8_t feels_like_temp, humidity, temp, wind;
    const char *country, *day;

    /* Open a cursor to search for the location. */
    error_check(session->open_cursor(session, "index:weather:country", NULL, NULL, &loc_cursor));
    loc_cursor->set_key(loc_cursor, country_average);
    ret = loc_cursor->search(loc_cursor);

    /*
     *  Error handling in the case RUS is not found. In this case as it's a hardcoded location,
     *  if there aren't any matching locations, no average data is obtained and we proceed with the
     *  test instead of aborting. If an unexpected error occurs, exit.
     */
    if (ret == WT_NOTFOUND)
        return;
    else if (ret != 0)
        exit(EXIT_FAILURE);

    /* Populate the array with the totals of each of the columns. */
    count = 0;
    memset(rec_arr, 0, sizeof(rec_arr));
    while (ret == 0) {
        error_check(loc_cursor->get_value(loc_cursor, &hour, &pressure, &loc_lat, &loc_long, &temp,
          &humidity, &wind, &feels_like_temp, &day, &country));

        if (strcmp(country, country_average) != 0) {
            ret = loc_cursor->next(loc_cursor);
            continue;
        }

        count++;
        /* Increment the values of the rec_arr with the temp_arr values. */
        rec_arr[0] += temp;
        rec_arr[1] += humidity;
        rec_arr[2] += pressure;
        rec_arr[3] += wind;
        rec_arr[4] += feels_like_temp;

        ret = loc_cursor->next(loc_cursor);
    }

    scan_end_check(ret == WT_NOTFOUND);
    error_check(loc_cursor->close(loc_cursor));

    /* Get the average values by dividing with the total number of records. */
    for (int i = 0; i < NUM_REC; i++)
        rec_arr[i] = rec_arr[i] / count;

    /* List the average records */
    printf(
      "Average records for location %s : \nTemp: %u"
      ", Humidity: %u"
      ", Pressure: %u"
      ", Wind: %u"
      ", Feels like: %u"
      "\n",
      country_average, rec_arr[0], rec_arr[1], rec_arr[2], rec_arr[3], rec_arr[4]);
}

/*! [col-store main] */
int
main(int argc, char *argv[])
{
    WT_CONNECTION *conn;
    WT_CURSOR *cursor;
    WT_SESSION *session;
    WEATHER weather_data[NUM_ENTRIES];
    char countries[][NUM_COUNTRIES - 1] = {"AUS", "GBR", "USA", "NZD", "IND", "CHI", "RUS"};
    int max_temp_result, min_temp_result, ret;
    uint16_t ending_time, starting_time;

    home = example_setup(argc, argv);

    /* Establishing a connection. */
    error_check(wiredtiger_open(home, NULL, "create,statistics=(fast)", &conn));

    /* Establishing a session. */
    error_check(conn->open_session(conn, NULL, NULL, &session));

    /*! [col-store create columns] */
    /* Create a table with columns and colgroups. */
    error_check(session->create(session, TABLE_NAME,
      "key_format=r,value_format=" WT_UNCHECKED_STRING(
        HHHHBBBB5S5S) ",columns=(id,hour,pressure,loc_lat,"
                      "loc_long,temp,humidity,"
                      "wind,feels_like_temp,day,country),colgroups=(day_time,temperature,"
                      "humidity_pressure,"
                      "wind,feels_like_temp,location)"));

    /* Create the colgroups */
    error_check(session->create(session, "colgroup:weather:day_time", "columns=(hour,day)"));
    error_check(session->create(session, "colgroup:weather:temperature", "columns=(temp)"));
    /*! [col-store create columns] */
    error_check(session->create(
      session, "colgroup:weather:humidity_pressure", "columns=(pressure,humidity)"));
    error_check(session->create(session, "colgroup:weather:wind", "columns=(wind)"));
    error_check(
      session->create(session, "colgroup:weather:feels_like_temp", "columns=(feels_like_temp)"));
    error_check(
      session->create(session, "colgroup:weather:location", "columns=(loc_lat,loc_long,country)"));

    /* Generating random data to populate the weather table. */
    generate_data(weather_data);

    /* Open a cursor on the table to insert the data. */
    error_check(session->open_cursor(session, TABLE_NAME, NULL, "append", &cursor));
    for (int i = 0; i < NUM_ENTRIES; i++) {
        cursor->set_value(cursor, weather_data[i].hour, weather_data[i].pressure,
          weather_data[i].loc_lat, weather_data[i].loc_long, weather_data[i].temp,
          weather_data[i].humidity, weather_data[i].wind, weather_data[i].feels_like_temp,
          weather_data[i].day, weather_data[i].country);
        error_check(cursor->insert(cursor));
    }
    /* Close cursor. */
    error_check(cursor->close(cursor));

    /* Prints all the data in the database. */
    print_all_columns(session);

    /* Create indexes for searching */
    error_check(session->create(session, "index:weather:hour", "columns=(hour)"));
    error_check(session->create(session, "index:weather:country", "columns=(country)"));

    /*
     * Start and end points for time range for finding min/max temperature, in 24 hour format.
     * Example uses 10am - 8pm but can change the values for desired start and end times.
     */
    starting_time = 1000;
    ending_time = 2000;
    min_temp_result = 0;
    max_temp_result = 0;
    ret = find_min_and_max_temp(
      session, starting_time, ending_time, &min_temp_result, &max_temp_result);

    /* If the min/max temperature is not found due to some error, there is no result to print. */
    if (ret == 0) {
        printf("The minimum temperature between %" PRIu16 " and %" PRIu16 " is %d.\n",
          starting_time, ending_time, min_temp_result);
        printf("The maximum temperature between %" PRIu16 " and %" PRIu16 " is %d.\n",
          starting_time, ending_time, max_temp_result);
    }

    /* Update the temperature from Celsius to Fahrenheit. */
    update_celsius_to_fahrenheit(session);

    /*
     * Start and end points for time range for finding min/max temperature, in 24 hour format.
     * Example uses 10am - 8pm but can change the values for desired start and end times.
     */
    starting_time = 1000;
    ending_time = 2000;
    min_temp_result = 0;
    max_temp_result = 0;
    ret = find_min_and_max_temp(
      session, starting_time, ending_time, &min_temp_result, &max_temp_result);

    /* If the min/max temperature is not found due to some error, there is no result to print. */
    if (ret == 0) {
        printf("The minimum temperature between %" PRIu16 " and %" PRIu16 " is %d.\n",
          starting_time, ending_time, min_temp_result);
        printf("The maximum temperature between %" PRIu16 " and %" PRIu16 " is %d.\n",
          starting_time, ending_time, max_temp_result);
    }

    printf("Average for all countries:\n");
    for (int i = 0; i < NUM_COUNTRIES; i++)
        average_data(session, countries[i]);

    remove_country(session);

    printf("Average for all countries:\n");
    for (int i = 0; i < NUM_COUNTRIES; i++)
        average_data(session, countries[i]);

    /* Close the connection. */
    error_check(conn->close(conn, NULL));
    return (EXIT_SUCCESS);
}

/*! [col-store main] */

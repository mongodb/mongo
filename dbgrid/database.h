// database.h

/* we create a database object for each client database 
*/
class Database { 
    static boost::mutex mutex;
public:
    /* load list of databases from the grid db. */
    static void load();
};

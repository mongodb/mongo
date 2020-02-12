# Memory Management

-   Avoid using bare pointers for dynamically allocated objects. Prefer `std::unique_ptr`,
    `std::shared_ptr`, or another RAII class such as `BSONObj`.
-   If you assign the output of `new/malloc()` directly to a bare pointer you should document where
    it gets deleted/freed, who owns it along the way, and how exception safety is ensured.

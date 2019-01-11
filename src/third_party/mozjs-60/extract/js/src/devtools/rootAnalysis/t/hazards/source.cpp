#define ANNOTATE(property) __attribute__((tag(property)))

struct Cell { int f; } ANNOTATE("GC Thing");

struct RootedCell { RootedCell(Cell*) {} } ANNOTATE("Rooted Pointer");

class AutoSuppressGC_Base {
  public:
    AutoSuppressGC_Base() {}
    ~AutoSuppressGC_Base() {}
} ANNOTATE("Suppress GC");

class AutoSuppressGC_Child : public AutoSuppressGC_Base {
  public:
    AutoSuppressGC_Child() : AutoSuppressGC_Base() {}
};

class AutoSuppressGC {
    AutoSuppressGC_Child helpImBeingSuppressed;

  public:
    AutoSuppressGC() {}
};

extern void GC() ANNOTATE("GC Call");
extern void invisible();

void GC()
{
    // If the implementation is too trivial, the function body won't be emitted at all.
    asm("");
    invisible();
}

extern void usecell(Cell*);

void suppressedFunction() {
    GC(); // Calls GC, but is always called within AutoSuppressGC
}

void halfSuppressedFunction() {
    GC(); // Calls GC, but is sometimes called within AutoSuppressGC
}

void unsuppressedFunction() {
    GC(); // Calls GC, never within AutoSuppressGC
}

volatile static int x = 3;
volatile static int* xp = &x;
struct GCInDestructor {
    ~GCInDestructor() {
        invisible();
        asm("");
        *xp = 4;
        GC();
    }
};

Cell*
f()
{
    GCInDestructor kaboom;

    Cell cell;
    Cell* cell1 = &cell;
    Cell* cell2 = &cell;
    Cell* cell3 = &cell;
    Cell* cell4 = &cell;
    {
        AutoSuppressGC nogc;
        suppressedFunction();
        halfSuppressedFunction();
    }
    usecell(cell1);
    halfSuppressedFunction();
    usecell(cell2);
    unsuppressedFunction();
    {
        // Old bug: it would look from the first AutoSuppressGC constructor it
        // found to the last destructor. This statement *should* have no effect.
        AutoSuppressGC nogc;
    }
    usecell(cell3);
    Cell* cell5 = &cell;
    usecell(cell5);

    // Hazard in return value due to ~GCInDestructor
    Cell* cell6 = &cell;
    return cell6;
}

Cell* copy_and_gc(Cell* src)
{
    GC();
    return reinterpret_cast<Cell*>(88);
}

void use(Cell* cell)
{
    static int x = 0;
    if (cell)
        x++;
}

struct CellContainer {
    Cell* cell;
    CellContainer() {
        asm("");
    }
};

void loopy()
{
    Cell cell;

    // No hazard: haz1 is not live during call to copy_and_gc.
    Cell* haz1;
    for (int i = 0; i < 10; i++) {
        haz1 = copy_and_gc(haz1);
    }

    // No hazard: haz2 is live up to just before the GC, and starting at the
    // next statement after it, but not across the GC.
    Cell* haz2 = &cell;
    for (int j = 0; j < 10; j++) {
        use(haz2);
        GC();
        haz2 = &cell;
    }

    // Hazard: haz3 is live from the final statement in one iteration, across
    // the GC in the next, to the use in the 2nd statement.
    Cell* haz3;
    for (int k = 0; k < 10; k++) {
        GC();
        use(haz3);
        haz3 = &cell;
    }

    // Hazard: haz4 is live across a GC hidden in a loop.
    Cell* haz4 = &cell;
    for (int i2 = 0; i2 < 10; i2++) {
        GC();
    }
    use(haz4);

    // Hazard: haz5 is live from within a loop across a GC.
    Cell* haz5;
    for (int i3 = 0; i3 < 10; i3++) {
        haz5 = &cell;
    }
    GC();
    use(haz5);

    // No hazard: similar to the haz3 case, but verifying that we do not get
    // into an infinite loop.
    Cell* haz6;
    for (int i4 = 0; i4 < 10; i4++) {
        GC();
        haz6 = &cell;
    }

    // No hazard: haz7 is constructed within the body, so it can't make a
    // hazard across iterations. Note that this requires CellContainer to have
    // a constructor, because otherwise the analysis doesn't see where
    // variables are declared. (With the constructor, it knows that
    // construction of haz7 obliterates any previous value it might have had.
    // Not that that's possible given its scope, but the analysis doesn't get
    // that information.)
    for (int i5 = 0; i5 < 10; i5++) {
        GC();
        CellContainer haz7;
        use(haz7.cell);
        haz7.cell = &cell;
    }

    // Hazard: make sure we *can* see hazards across iterations involving
    // CellContainer;
    CellContainer haz8;
    for (int i6 = 0; i6 < 10; i6++) {
        GC();
        use(haz8.cell);
        haz8.cell = &cell;
    }
}

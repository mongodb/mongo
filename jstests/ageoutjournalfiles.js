if (false && db.serverStatus().dur) {

    assert(db.serverStatus().dur.ageOutJournalFiles != false);

    db.adminCommand({ setParameter: 1, ageOutJournalFiles: false });

    assert(db.serverStatus().dur.ageOutJournalFiles == false);

    db.adminCommand({ setParameter: 1, ageOutJournalFiles: true });

    assert(db.serverStatus().dur.ageOutJournalFiles != false);

}
else {
//    print("dur is off");
}
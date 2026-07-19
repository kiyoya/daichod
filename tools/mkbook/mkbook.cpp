// Creates an empty SQL-backed GnuCash book at the given URI and prints the
// root account GUID. Test infrastructure: gnucash-cli cannot create books,
// and the shim itself refuses to (creation is a deployment act, not
// mechanical engine access on an existing book).

#include <cstdio>
#include <string>

#include <Account.h>
#include <qof.h>

#include "engine/map.h"
#include "engine/session.h"
#include "rpc/error.h"

int main(int argc, char** argv) {
  if (argc != 2) {
    std::fprintf(stderr, "usage: daichod-mkbook <sqlite3:///abs/path>\n");
    return 1;
  }
  const std::string uri = argv[1];

  daichod::Session::GlobalInit();

  QofSession* session = qof_session_new(qof_book_new());
  qof_session_begin(session, uri.c_str(), SESSION_NEW_STORE);
  if (qof_session_get_error(session) != ERR_BACKEND_NO_ERR) {
    std::fprintf(stderr, "begin failed: %s\n",
                 qof_session_get_error_message(session));
    return 1;
  }

  QofBook* book = qof_session_get_book(session);
  Account* root = gnc_book_get_root_account(book);
  qof_session_save(session, nullptr);
  if (qof_session_get_error(session) != ERR_BACKEND_NO_ERR) {
    std::fprintf(stderr, "save failed: %s\n",
                 qof_session_get_error_message(session));
    return 1;
  }

  std::printf("%s\n",
              daichod::GuidToString(qof_instance_get_guid(root)).c_str());

  qof_session_end(session);
  qof_session_destroy(session);
  daichod::Session::GlobalShutdown();
  return 0;
}

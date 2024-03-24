#include "process_queries.h"

using namespace std;

vector<vector<Document>> ProcessQueries( const SearchServer& search_server, const vector<string>& queries) {
    vector<vector<Document>> documents_for_queries(queries.size());
    transform(execution::par, queries.begin(), queries.end(), documents_for_queries.begin(),
        [&search_server](auto raw_query) { return search_server.FindTopDocuments(raw_query, DocumentStatus::ACTUAL); });
    return documents_for_queries;
}
vector<Document> ProcessQueriesJoined(const SearchServer& search_server, const vector<string>& queries) {
    vector<Document> documents_for_flat_quries;
    for (const auto& documents : ProcessQueries(search_server, queries)) {
        documents_for_flat_quries.insert(documents_for_flat_quries.end(),
            make_move_iterator(documents.begin()), make_move_iterator(documents.end()));
    }
    return documents_for_flat_quries;
}
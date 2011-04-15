#include <iostream>
#include <cstdlib>
#include <ctime>
#include <assert.h>
#include "src/ncbidata.hh"
#include "src/accessconv.hh"
#include "src/taxonomyinterface.hh"
#include "src/constants.hh"
// #include "src/tree_util.hh"



using namespace std;



bool unittest_assert( bool condition, const std::string& testname ) {
	if( !condition ) {
		std::cerr << "Test " << testname << " failed!" << std::endl;
		assert( condition );
	} else {
// 		std::cout << "Test " << testname << " succeeded" << std::endl;
	}
	return condition;
}

int main( int argc, char** argv ) {

	// command line arguments
	if( argc < 2 ) {
		std::cerr << "Not enough parameters given. Usage:" << std::endl << argv[0] << " seqid2taxid.sqlite" << std::endl;
		return EXIT_FAILURE;
	}

	const std::string accessconverter_filename = argv[1];
	bool alltests = true;
	TaxonTree::iterator node_it;


	{ //basic tests on unmodified taxonomy
		Taxonomy* tax = loadTaxonomyFromEnvironment( &default_ranks );
		TaxonomyInterface taxinter( tax );
		const TaxonNode* root_node = taxinter.getRoot();
		cerr << "taxonomy size: " << tax->size() << " nodes" << endl;

		// check index and taxonomy size
		int number_nodes = tax->size();
		alltests = alltests && unittest_assert( number_nodes == tax->indexSize(), "TAXONOMY_SIZE" );

		// check that all annotations are set correctly
		for( node_it = tax->begin(); node_it != tax->end(); ++node_it ) {
			alltests = alltests && unittest_assert( ! (*node_it)->annotation || (*node_it)->annotation->name != (*node_it)->annotation->rank, "ANNOTATION_POINTER" );
		}

		// pick random nodes and check nested set values for children
		srand((unsigned)time(0));

		for(int i=0; i < 1000; ) {
			node_it = tax->begin();
			node_it += rand() % number_nodes;
	// 		std::cout << "node picked: " << (*node_it)->annotation->name << std::endl;
			unsigned int leftvalue = (*node_it)->leftvalue;
			unsigned int rightvalue = (*node_it)->rightvalue;
			for( TaxonTree::sibling_iterator child = node_it.node->first_child; child != node_it.node->last_child; ++child ) {
	// 			std::cout << "child" << std::endl;
				alltests = alltests && unittest_assert( leftvalue <= (*child)->leftvalue && rightvalue >= (*child)->rightvalue, "NESTED_SET (" + ( (*child)->annotation ? (*child)->annotation->name : "dummy node" ) + ")" );
				++i; //increase for every child
			}
		}

		// test depth information
		alltests = alltests && unittest_assert( taxinter.getPathLength( root_node, root_node ) == std::make_pair( 0, 0 ), "PATHLENGTH_ROOT_TO_ROOT" );
		alltests = alltests && unittest_assert( root_node->data->root_pathlength == 0, "PATHLENGTH_ROOT_TO_ROOT" );

		for(int i=0; i < 1000; ) {
			node_it = tax->begin();
			node_it += ( rand() + 1 ) % number_nodes;
			TaxonTree::fixed_depth_iterator fix_it( node_it );
			unsigned int depth = (*fix_it)->root_pathlength;
			while( fix_it != fix_it.end() ) {
				alltests = alltests && unittest_assert( (*fix_it)->root_pathlength == depth, "PATHLENGTH_RANDOM_TO_ROOT_FIXED_DEPTH (" + ( (*fix_it)->annotation ? (*fix_it)->annotation->name : "dummy node" ) + ")" );
				++fix_it;
				++i;
			}
		}

		//check for right depth information
		for( node_it = ++( tax->begin() ); node_it != tax->end(); ++node_it ) {
			alltests = alltests && unittest_assert( node_it.node->parent->data->root_pathlength + 1 == (*node_it)->root_pathlength, "PATHLENGTH_TO_PARENT_EQUALS_ONE (" +  ( (*node_it)->annotation ? (*node_it)->annotation->name : "dummy node" ) + ")" );
			if( ! alltests ) {
				std::cerr << "node: " << (*node_it)->annotation->name << " with root path length " << (*node_it)->root_pathlength << " and parent root path length " << node_it.node->parent->data->root_pathlength << std::endl;
				return 0;
			}
		}

		//check whether unclassified nodes are marked correctly
		for( node_it = ++( tax->begin() ); node_it != tax->end(); ++node_it ) {
			TaxonNode* node = node_it.node;
			if( node->data->is_unclassified ) {
				TaxonNode* tmp_node;
				for( tmp_node = node; tmp_node != root_node; tmp_node = tmp_node->parent ) {
// 					std::cerr << "marked as unclassified: " << tmp_node->data->annotation->name << std::endl;
					if( tmp_node->data->annotation && tmp_node->data->annotation->name.find( "unclassified" ) != std::string::npos ) {
						break;
					}
				}
				alltests = alltests && unittest_assert( tmp_node != root_node, "UNCLASSIFIED_MARKED (" + node->data->annotation->name + ")" );
			}
		}

		TaxonNode* node = taxinter.getNode( 166532 );
		if( node ) {
			alltests = alltests && unittest_assert( node->data->is_unclassified, "UNCLASSIFIED_MARKED (unclassified Potamonautes)" );
		}
		node = taxinter.getNode( 713063 );
		if( node ) {
			alltests = alltests && unittest_assert( node->data->is_unclassified, "UNCLASSIFIED_MARKED (unclassified Tenericutes)" );
		}
		node = taxinter.getNode( 39945 );
		if( node ) {
			alltests = alltests && unittest_assert( node->data->is_unclassified, "UNCLASSIFIED_MARKED (unclassified Mollicutes)" );
		}
		node = taxinter.getNode( 575771 );
		if( node ) {
			alltests = alltests && unittest_assert( node->data->is_unclassified, "UNCLASSIFIED_MARKED (Candidatus Lumbricincola sp. Ef-1)" );
		}

		delete tax;
	}

	{
		Taxonomy* tax = loadTaxonomyFromEnvironment( &default_ranks );
		int number_nodes = tax->size();
		tax->deleteUnmarkedNodes();
		tax->setRankDistances( default_ranks );
		cerr << "deleting unmarked nodes succeeded, " << number_nodes - tax->size() << " nodes deleted" << endl;

		//check depth information for specic rank
		for( node_it = ++( tax->begin() ); node_it != tax->end(); ++node_it ) {
			TaxonNode* node = node_it.node;
			if( node->data->annotation ) {
				const string& rank = node->data->annotation->rank;

	// 			std::cerr << node->data->annotation->name << std::endl;
	// 			std::cerr << "rank: " << rank << " normalized depth: " << node->data->root_pathlength << std::endl;

				if ( rank == "superkingdom" ) {
					alltests = alltests && unittest_assert( node->data->root_pathlength == 1 , "NORMALIZED_DEPTH (" +  node->data->annotation->name + ")" );
				}
				if( rank == "phylum" ) {
					alltests = alltests && unittest_assert( node->data->root_pathlength == 2 , "NORMALIZED_DEPTH (" +  node->data->annotation->name + ")" );
				}
				if( rank == "class" ) {
					alltests = alltests && unittest_assert( node->data->root_pathlength == 3 , "NORMALIZED_DEPTH (" +  node->data->annotation->name + ")" );
				}
				if( rank == "order" ) {
					alltests = alltests && unittest_assert( node->data->root_pathlength == 4 , "NORMALIZED_DEPTH (" +  node->data->annotation->name + ")" );
				}
				if( rank == "family" ) {
					alltests = alltests && unittest_assert( node->data->root_pathlength == 5 , "NORMALIZED_DEPTH (" +  node->data->annotation->name + ")" );
				}
				if( rank == "genus" ) {
					alltests = alltests && unittest_assert( node->data->root_pathlength == 6 , "NORMALIZED_DEPTH (" +  node->data->annotation->name + ")" );
				}
				if( rank == "species" ) {
					alltests = alltests && unittest_assert( node->data->root_pathlength == 7 , "NORMALIZED_DEPTH (" +  node->data->annotation->name + ")" );
				}
			}
		}
		delete tax;
	}


	{ //check seqid-converter (not really taxonomy)
		// test sqlite seqid converter
		StrIDConverter* accessconv = loadStrIDConverterFromFile( accessconverter_filename );
		StrIDConverter& seqid2taxid = *accessconv;
	// 	gi2taxid.fail();
	// 	unittest_assert( gi2taxid[ 39 ] == 9913, "SQLITE_FIXED_GI_LOOKUP" );

		try {
			seqid2taxid[ "1000000" ];
		} catch( std::out_of_range e ) {
			alltests = alltests && unittest_assert( typeid( e ) == typeid( std::out_of_range ), "SQLITE_OUT_OF_RANGE" );
		} catch( std::exception e ) {
			alltests = alltests && unittest_assert( typeid( e ) == typeid( std::out_of_range ), "SQLITE_OUT_OF_RANGE" ); //why this?
		}
	}

	if( alltests ) {
		cout << std::endl << "All tests ran through!" << endl;
	} else {
		cerr << std::endl << "At least one test failed!" << endl;
	}

	// tidy up and quit
	return EXIT_SUCCESS;
}
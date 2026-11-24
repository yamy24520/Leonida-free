# Leonida-free

Leonida-free est une édition communautaire indépendante des outils et du moteur Leonida. **L’accès à nos sources, à nos corrections et à la participation au projet est gratuit, sans abonnement ni paiement.** Cette politique d’accès ne modifie pas les droits accordés par la GPL.

Le dépôt contient le code du runtime, le traducteur de shaders, les outils et leurs tests. Il **ne contient pas le jeu**, ses ressources, ses clés, un exécutable jouable ou des shaders issus d’une copie du jeu. Tester le code sur des données synthétiques ne démontre pas que le jeu démarre, affiche une image ou est jouable. Nous ne garantissons aucun de ces résultats.

## Essayer sans jeu

Depuis la racine, avec Git et Python 3, les commandes suivantes contrôlent les fichiers suivis, puis testent le décodage d’images DDS avec des données fabriquées en mémoire. Elles ne demandent ni jeu ni GPU.

```sh
python -I scripts/check_no_game_data.py
python -I -B tests/test_dds_png.py
python -I -B tests/test_dds_png_dx10.py
```

Le premier contrôle reste silencieux s’il réussit. Les deux suites DDS doivent afficher le nombre de tests et `OK`. Pour contribuer, voir [le guide](CONTRIBUTING.md).

## D’où vient cette édition ?

Le **24 novembre 2026**, nous avons repris une sélection de sources de [Leonida, révision `71d1ab1f`](https://github.com/ASTROWwwW/Leonida/commit/71d1ab1f818c8525e97c62011391b5dd2a3cbee8) sur un nouvel historique. Le moteur et les tests retenus correspondent aux fichiers publics de cette révision ; aucun résultat d’essai privé n’est inclus. Le dossier `game/` contient uniquement un manifeste public d’identification (noms, tailles et empreintes), pas les fichiers qu’il décrit. Le logo, les rapports de mesure, l’ancien suivi du projet et les ressources propriétaires ne font pas partie de cette édition. Le sous-module optionnel `rpf9-explorer` n’est qu’une référence à un dépôt externe : il n’est pas inclus dans ce dépôt et n’est pas nécessaire aux tests ci-dessus.

Ce projet n’est ni officiel ni affilié à Rockstar Games, Take-Two Interactive ou Microsoft. Les noms et marques appartiennent à leurs titulaires. Le code Leonida est distribué sous [GNU GPL version 3 ou toute version ultérieure](LICENSE) ; les composants tiers gardent leur propre licence. Voir [les crédits et la provenance](CREDITS.md).

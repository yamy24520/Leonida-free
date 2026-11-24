# Contribuer à Leonida-free

Proposez une correction, une documentation ou un test via une branche et une demande de fusion. Aucune participation financière n’est requise. Expliquez le problème, le comportement attendu et le résultat réellement observé ; pour une régression, joignez si possible un cas synthétique.

1. Avant de modifier le code, reproduisez le comportement sur une entrée créée pour le test. N’utilisez pas de fichiers du jeu ni de SDK propriétaire dans un exemple.
2. Faites un changement ciblé. Ajoutez un test et conservez la provenance, la licence et les notices de tout travail repris. Distinguez toujours une validation CPU d’une observation GPU ou d’une image effectivement présentée.
3. Depuis la racine, exécutez le test concerné. Par exemple, sans jeu ni GPU :
   ```sh
   python -I -B tests/test_dds_png.py
   python -I -B tests/test_dds_png_dx10.py
   ```
4. Après avoir ajouté explicitement les fichiers prévus à l’index Git, contrôlez ce qui sera publié :
   ```sh
   git diff --cached --stat
   git diff --cached --check
   python -I scripts/check_no_game_data.py
   ```
   Ce dernier contrôle reconnaît des formats, pas la licence ou l’origine des fichiers. Vérifiez aussi manuellement les fichiers texte, les droits de redistribution et les informations sensibles.

Ne joignez jamais de binaire du jeu, d’archive, de contenu extrait ou traduit depuis le jeu, de capture du jeu, de clés, de jetons, de chemins personnels, de journaux de session ni de SDK propriétaire. Préférez des compteurs agrégés et des fixtures synthétiques dont vous avez les droits.
